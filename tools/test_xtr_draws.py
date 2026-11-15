#!/usr/bin/env python3
"""test_xtr_draws.py -- unit + counterexample suite for tools/xtr_draws.py (TOOL-1).

Builds SYNTHETIC .xtr traces byte by byte (record layouts per Xenia
src/xenia/gpu/trace_protocol.h @ 0b4e6c440e91f118b42a5e9e32a6b629ce4c387c; the
builder pattern follows MCLA's xtr_dump_validator.cpp self-test @
5aa1969604b13fe44b5acc81afbed2943926a4fa) into a temp directory - never the
repo - and checks the reader against them: header, register snapshots, packet
start/end, raw and snappy memory, immediate and memory shader loads, indirect
buffers (dispatch consumed once, never re-executed), draw indexing, frame
limits, constants, fetches, resolves, the JSON summary, and malformed /
truncated counterexamples that MUST refuse with the right exit code at the
right byte offset.

The module under test is tools/xtr_draws.py next to this file, or the path in
$XTR_DRAWS_PATH (the mutation harness points it at a temp copy).

Verdict line (machine-read by tools/mutate_xtr_draws.py):
  XTR_DRAWS TESTS: total=N passed=P failed=F exceptions=E skipped=S
A test "fails" only by an explicit assertion; an exception escaping a test is
counted separately as an exception (a crash is a missing verdict, not a
finding). Exit 0 iff failed == 0 and exceptions == 0.
"""
import collections
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
MOD_PATH = os.environ.get('XTR_DRAWS_PATH') or os.path.join(HERE, 'xtr_draws.py')


def load_module(path):
    spec = importlib.util.spec_from_file_location('xtr_draws_under_test', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


X = load_module(MOD_PATH)
try:
    import cramjam
except ImportError:
    cramjam = None
try:
    import xxhash
except ImportError:
    xxhash = None

TMP = None
XENIA_SHA = b'0b4e6c440e91f118b42a5e9e32a6b629ce4c387c'
TITLE = 0x454107ED

# ── synthetic trace builder ──────────────────────────────────────────────────


class Trace:
    def __init__(self, version=1, sha=XENIA_SHA, title=TITLE, header=True):
        self.b = bytearray()
        if header:
            self.b += struct.pack('<I40sI', version, sha, title)

    def off(self):
        return len(self.b)

    def raw(self, data):
        self.b += data
        return self

    def tag(self, t):
        return self.raw(struct.pack('<I', t))

    def prim_start(self, base=0x1F000000, count=0):
        return self.tag(0).raw(struct.pack('<II', base, count))

    def prim_end(self):
        return self.tag(1)

    def ib_start(self, base=0x1F100000, count=0):
        return self.tag(2).raw(struct.pack('<II', base, count))

    def ib_end(self):
        return self.tag(3)

    def packet_start(self, dwords, base=0x1F000100, count=None):
        body = b''.join(struct.pack('>I', d & 0xFFFFFFFF) for d in dwords)
        n = len(dwords) if count is None else count
        return self.tag(4).raw(struct.pack('<II', base, n)).raw(body)

    def packet_end(self):
        return self.tag(5)

    def packet(self, dwords, base=0x1F000100):
        return self.packet_start(dwords, base).packet_end()

    def _mem(self, tag, base, payload, enc, dlen, elen):
        return self.tag(tag).raw(struct.pack(
            '<IIII', base, enc, len(payload) if elen is None else elen,
            len(payload) if dlen is None else dlen)).raw(payload)

    def mem_read(self, base, payload, enc=0, dlen=None, elen=None):
        return self._mem(6, base, payload, enc, dlen, elen)

    def mem_write(self, base, payload, enc=0, dlen=None, elen=None):
        return self._mem(7, base, payload, enc, dlen, elen)

    def edram(self, payload=b'', enc=0):
        return self.tag(8).raw(struct.pack('<II', enc, len(payload))).raw(payload)

    def event(self, et=0):
        return self.tag(9).raw(struct.pack('<I', et))

    def regs(self, first, values, enc=0, payload=None, count=None, elen=None):
        data = struct.pack('<%dI' % len(values), *values) if payload is None else payload
        n = len(values) if count is None else count
        return self.tag(10).raw(struct.pack('<IIIII', first, n, 0, enc,
                                            len(data) if elen is None else elen)).raw(data)

    def gamma(self, rw=0, payload=b'', enc=0):
        return self.tag(11).raw(struct.pack('<BxxxII', rw, enc, len(payload))).raw(payload)

    def save(self, name):
        path = os.path.join(TMP, name + '.xtr')
        with open(path, 'wb') as f:
            f.write(self.b)
        return path


# ── PM4 helpers (big-endian dwords, layouts per pm4_command_processor_implement.h)

def t0(base, *vals, one=False):
    return [((len(vals) - 1) << 16) | (int(one) << 15) | base, *vals]


def t3(op, *body):
    return [0xC0000000 | ((len(body) - 1) << 16) | (op << 8), *body]


def init(prim=4, src=2, count=3, idx32=0):
    return prim | (src << 6) | (idx32 << 11) | (count << 16)


def draw2(prim=4, count=3, src=2):
    return t3(0x36, init(prim, src, count))


def draw1(prim=4, count=3, src=2, viz=0, dma=None):
    return t3(0x22, viz, init(prim, src, count), *(list(dma) if dma else []))


def set_const(ctype, index, *vals):
    return t3(0x2D, (ctype << 16) | index, *vals)


def set_const2(index, *vals):
    return t3(0x55, index, *vals)


def load_alu(addr, ctype, index, size):
    return t3(0x2F, addr, (ctype << 16) | index, size)


def im_load_imm(stype, dwords):
    return t3(0x2B, stype, len(dwords), *dwords)


def im_load(stype, addr, size):
    return t3(0x27, addr | stype, size)


def event_write(initiator):
    return t3(0x46, initiator)


def event_write_ext(initiator=0x18, addr=0x1F300000):
    return t3(0x5A, initiator, addr)


def ib_dispatch_short(list_ptr):
    return [0xC0013F00, list_ptr]        # the writer's two-dword record of a 3-dword packet


def ib_pfd(list_ptr, length):
    return t3(0x37, list_ptr, length)


def swap():
    return t3(0x64, 0x53574150, 0x1F130000, 0x500, 0x2D0)


def fl(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def be_bytes(dwords):
    return b''.join(struct.pack('>I', d & 0xFFFFFFFF) for d in dwords)


# ── ucode helpers (ucode.h layouts) ──────────────────────────────────────────

def cf(op, address, count=0, sequence=0):
    return (address | (count << 12) | (sequence << 16), op << 12)


def cf_pair(a, b=(0, 0)):
    d0 = a[0]
    d1 = (a[1] & 0xFFFF) | ((b[0] & 0xFFFF) << 16)
    d2 = (b[0] >> 16) | ((b[1] & 0xFFFF) << 16)
    return [d0 & 0xFFFFFFFF, d1 & 0xFFFFFFFF, d2 & 0xFFFFFFFF]


def vfetch(dst=1, fmt=57, stride=3, offset=0, const=0, sel=0, must_be_one=1,
           swiz=0xE88, mini=0, src=0, signed=0, nf=0, exp_adjust=0, opcode=0):
    d0 = opcode | (src << 5) | (dst << 12) | (must_be_one << 19) | (const << 20) | (sel << 25)
    d1 = swiz | (signed << 12) | (nf << 13) | (fmt << 16) | ((exp_adjust & 0x3F) << 24) | (mini << 30)
    d2 = stride | ((offset & 0x7FFFFF) << 8)
    return [d0, d1, d2]


def vs_simple(fmt=57, stride=3):
    return cf_pair(cf(2, 1, 1, 1)) + vfetch(fmt=fmt, stride=stride)


# ── the runner ───────────────────────────────────────────────────────────────

TESTS = []


def test(name):
    def deco(fn):
        TESTS.append((name, fn))
        return fn
    return deco


class Fail(Exception):
    pass


class Skip(Exception):
    pass


def ck(cond, msg):
    if not cond:
        raise Fail(msg)


def eq(a, b, what):
    ck(a == b, '%s: got %r, expected %r' % (what, a, b))


def expect_refusal(path, code, offset=None, **kw):
    """The fail-closed contract: a NAMED refusal (TraceError) with this exit
    code (and byte offset). Any other exception is an assertion failure -
    the tool crashed instead of refusing."""
    try:
        X.parse(path, **kw)
    except X.TraceError as e:
        eq(e.code, code, 'exit code (%s)' % e.reason)
        if offset is not None:
            eq(e.offset, offset, 'refusal offset (%s)' % e.reason)
        return e
    except Exception as e:  # noqa: BLE001 - the contract under test
        raise Fail('expected a TraceError(exit %d) but the tool raised %s: %s'
                   % (code, type(e).__name__, e))
    raise Fail('parsed without refusal (expected exit %d)' % code)


def parse(path, **kw):
    """Positive path: the trace MUST parse. A refusal here is an assertion
    failure with the refusal's own words, not an escaped exception."""
    try:
        return X.parse(path, **kw)
    except X.TraceError as e:
        raise Fail('expected a clean parse, got refusal exit %d at offset %d: %s'
                   % (e.code, e.offset, e.reason))


def summary(path, **kw):
    eng, hdr, st = parse(path, **kw)
    return X.summary_dict(path, hdr, eng, st), eng


def summary5(path, **kw):
    """The accepted /5 projection (TOOL-3A semantics) rebuilt from the /6
    normalized summary by the tool's own expander, alongside the /6 keys."""
    s, eng = summary(path, **kw)
    s5 = dict(s)
    s5.update(X.expand_static_bindings_v5(json.loads(json.dumps(s))))
    return s5, eng


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = X.main(argv)
    except Exception as e:  # noqa: BLE001 - main() must never leak an exception
        raise Fail('main(%r) raised %s: %s' % (argv, type(e).__name__, e))
    return rc, out.getvalue(), err.getvalue()


def base_trace(mode=4):
    return Trace().regs(0x2208, [mode]).prim_start()


# ── header / record framing ──────────────────────────────────────────────────

@test('header parsed: version, build sha, title, one swap')
def _():
    p = Trace().regs(0, [0] * 8).event(0).save('hdr')
    eng, hdr, st = parse(p)
    eq(hdr['version'], 1, 'version')
    eq(hdr['sha'], XENIA_SHA.decode(), 'sha')
    eq(hdr['title'], TITLE, 'title')
    eq(eng.frame, 1, 'frames')
    eq(st.registers_records, 1, 'registers records')
    eq(st.bytes_parsed, os.path.getsize(p), 'bytes parsed')


@test('header truncated -> exit 2 at offset 0')
def _():
    p = Trace(header=False).raw(b'\0' * 47).save('hdr_short')
    expect_refusal(p, X.EXIT_MALFORMED, 0)


@test('header version 2 / big-endian header -> exit 4 at offset 0')
def _():
    expect_refusal(Trace(version=2).save('hdr_v2'), X.EXIT_UNSUPPORTED, 0)
    expect_refusal(Trace(version=1 << 24).save('hdr_be'), X.EXIT_UNSUPPORTED, 0)


@test('unknown record tag -> exit 4 at the record offset')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.tag(12).save('tag12'), X.EXIT_UNSUPPORTED, off)


@test('unknown event type -> exit 4 at the record offset')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.event(1).save('event1'), X.EXIT_UNSUPPORTED, off)


@test('truncated tag / truncated event body -> exit 2 at the record offset')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.raw(b'\x09\x00').save('tag_trunc'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.tag(9).save('event_trunc'), X.EXIT_MALFORMED, off)


@test('buffer-start with nonzero count -> exit 4 (writer emits 0)')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.prim_start(count=4).save('prim_count'), X.EXIT_UNSUPPORTED, off)


# ── registers ────────────────────────────────────────────────────────────────

@test('register snapshot applied; full 0x5003-register snapshot accepted')
def _():
    p = Trace().regs(0x2208, [6]).packet(draw2()).save('regs_applied')
    eng, _, _ = parse(p)
    eq(eng.draws[0]['kind'], 'resolve', 'kind from snapshot RB_MODECONTROL=6')
    p = Trace().regs(0, [0] * 0x5003).save('regs_full')
    eng, _, st = parse(p)
    eq(st.register_values, 0x5003, 'register values')


@test('register snapshot beyond the register file -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.regs(0x5000, [0] * 4).save('regs_oob'), X.EXIT_MALFORMED, off)


@test('register payload shorter than count*4 -> exit 2; encoded length past EOF -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.regs(0, [1, 2, 3, 4], payload=struct.pack('<3I', 1, 2, 3), count=4)
                   .save('regs_short'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.regs(0, [1], elen=1000).save('regs_elen'), X.EXIT_MALFORMED, off)


# ── packets ──────────────────────────────────────────────────────────────────

@test('packet count 0 / count beyond file / truncated body -> exit 2 at the record')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.packet_start([], count=0).save('pkt_zero'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.packet_start([0x80000000], count=1000).save('pkt_big'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    t.tag(4).raw(struct.pack('<II', 0x1F000100, 4)).raw(b'\0' * 8)
    expect_refusal(t.save('pkt_trunc'), X.EXIT_MALFORMED, off)


@test('PacketEnd without PacketStart -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.packet_end().save('end_no_start'), X.EXIT_MALFORMED, off)


@test('PacketStart while another packet is pending -> exit 2 (nothing dropped silently)')
def _():
    t = Trace().packet_start(draw2())
    off = t.off()
    expect_refusal(t.packet_start(draw2()).save('start_pending'), X.EXIT_MALFORMED, off)


@test('type-0 writes are big-endian; write_one repeats the base register')
def _():
    p = base_trace().packet(t0(0x2208, 6)).packet(t0(0x2100, 11, 22, 33, one=True)) \
        .packet(t0(0x2110, fl(1.5), fl(2.5))).packet(draw2()).save('t0')
    eng, _, _ = parse(p)
    d = eng.draws[0]
    eq(d['kind'], 'resolve', 'RB_MODECONTROL written by type-0')
    eq(d['regs'][0x2100], 33, 'write_one: last value at base')
    eq(d['regs'][0x2101], 0, 'write_one: base+1 untouched')
    eq(X.f32(d['regs'][0x2110]), 1.5, 'float register')
    eq(X.f32(d['regs'][0x2111]), 2.5, 'float register +1')


@test('type-0 body overflowing the record -> exit 2 at the record')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.packet([(3 << 16) | 0x2208, 1, 2]).save('t0_over'), X.EXIT_MALFORMED, off)


@test('type-1 writes two 11-bit registers; type-2 is a no-op')
def _():
    p = Trace().packet([0x40000000 | 0x100 | (0x101 << 11), 5, 7]).packet([0x80000000]).save('t1')
    eng, _, _ = parse(p)
    eq(eng.rd(0x100), 5, 'type-1 reg1')
    eq(eng.rd(0x101), 7, 'type-1 reg2')
    eq(eng.packets_executed, 2, 'packets executed')


@test('type-3 body overflowing the record -> exit 2 (only 0x3F may be short)')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.packet([0xC0030000 | (0x36 << 8), 1]).save('t3_over'), X.EXIT_MALFORMED, off)


@test('type-3 IM_LOAD_IMMEDIATE declaring more dwords than the packet -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.packet(t3(0x2B, 0, 10, 1, 2, 3)).save('imm_over'), X.EXIT_MALFORMED, off)


@test('unknown type-3 opcode counted, binned draws warned, XE_SWAP ignored')
def _():
    p = base_trace().packet(t3(0x7F, 0)).packet(t3(0x34, 0, 0)).packet(swap()).save('ops')
    s, eng = summary(p)
    eq(s['unknown_opcodes'], {'0x7F': 1}, 'unknown opcodes')
    eq(s['draw_packets'], 0, 'binned draw not decoded as a draw')
    ck('bin_draw' in s['warning_counts'], 'binned draw warned')
    eq(s['opcode_histogram'].get('0x64'), 1, 'swap counted in histogram')


# ── draws ────────────────────────────────────────────────────────────────────

@test('DRAW_INDX_2: ordinal, frame, prim, count, source select, opcode')
def _():
    p = base_trace().packet(draw2(prim=4, count=6)).save('draw2')
    eng, _, _ = parse(p)
    eq(len(eng.draws), 1, 'draw packets')
    d = eng.draws[0]
    eq((d['ord'], d['frame'], d['kind'], d['prim'], d['count'], d['src_sel'], d['op']),
       (1, 0, 'draw', 4, 6, 2, 0x36), 'draw fields')
    eq(d['regs'][0x21FC], init(4, 2, 6), 'VGT_DRAW_INITIATOR written')


@test('DRAW_INDX: viz dword skipped, initiator decoded, kDMA base/size written')
def _():
    p = base_trace().packet(draw1(prim=5, count=9, src=0, viz=0xDEADBEEF,
                                  dma=(0x1F200000, 0x40))).save('draw1')
    eng, _, _ = parse(p)
    d = eng.draws[0]
    eq((d['prim'], d['count'], d['src_sel'], d['op']), (5, 9, 0, 0x22), 'draw fields')
    eq(d['dma_base'], 0x1F200000, 'dma base')
    eq(d['dma_size'], 0x40, 'dma size')
    eq(eng.rd(0x21FD), 0x1F200000, 'VGT_DMA_BASE')
    eq(eng.rd(0x21FE), 0x40, 'VGT_DMA_SIZE')


@test('zero-index draw is counted AND flagged, never dropped')
def _():
    s, eng = summary(base_trace().packet(draw2(count=0)).save('draw_zero'))
    eq(s['draw_packets'], 1, 'draw packets')
    eq(s['zero_index_draws'], 1, 'zero-index flagged')


@test('RB_MODECONTROL classifies every draw packet: 4/5 draw, 6 resolve, 0 noop, else unknown')
def _():
    t = Trace()
    for mode in (4, 5, 6, 0, 3):
        t.packet(t0(0x2208, mode)).packet(draw2())
    s, eng = summary(t.save('modes'))
    eq([d['kind'] for d in eng.draws], ['draw', 'draw', 'resolve', 'noop', 'unknown_mode'], 'kinds')
    eq((s['draw_packets'], s['draws'], s['resolves'], s['noop_draws'], s['unknown_mode_draws']),
       (5, 2, 1, 1, 1), 'summary counts')
    eq(eng.resolves[0]['draw_ord'], 3, 'resolve points at draw #3')


@test('resolve record decodes RB_COPY_CONTROL / DEST_INFO / DEST_PITCH')
def _():
    p = Trace().packet(t0(0x2318, 4 | (1 << 8) | (1 << 20))) \
        .packet(t0(0x2319, 0x1D46C000)).packet(t0(0x231A, 1280 | (720 << 16))) \
        .packet(t0(0x231B, 2 | (6 << 7) | (0x3F << 16) | (1 << 24))) \
        .packet(t0(0x2208, 6)).packet(draw2()).save('resolve_regs')
    eng, _, _ = parse(p)
    r = eng.resolves[0]
    eq(r['dest_base'], 0x1D46C000, 'dest base')
    eq((r['copy']['copy_src_select'], r['copy']['color_clear_enable'], r['copy']['copy_command'],
        r['copy']['copy_command_name']), (4, 1, 1, 'convert'), 'copy control')
    eq((r['dest_pitch_decoded']['pitch'], r['dest_pitch_decoded']['height']), (1280, 720), 'pitch/height')
    eq((r['dest']['endian'], r['dest']['fmt'], r['dest']['exp_bias'], r['dest']['swap']),
       (2, 6, -1, 1), 'dest info')
    eq((r['source']['kind'], r['source']['index'], r['source']['register']), ('depth', 4, 0x2002),
       'copy_src_select 4 is the depth buffer')


@test('EVENT_WRITE / EVENT_WRITE_EXT / EVENT_WRITE_CFL / CONTEXT_UPDATE are NOT resolves')
def _():
    p = base_trace().packet(event_write(6)).packet(event_write_ext()) \
        .packet(t3(0x59, 0x18, 0, 0)).packet(t3(0x5E, 0x10)).packet(draw2()).save('events')
    s, eng = summary(p)
    eq(s['resolves'], 0, 'resolves')
    eq(len(eng.resolves), 0, 'resolve records')
    eq(s['screen_extent_events'], 1, 'screen-extent events')
    eq(s['event_writes'], {'6': 1}, 'VGT_EVENT_INITIATOR histogram')
    eq(s['draws'], 1, 'draws')
    eq(s['unknown_opcodes'], {}, 'no unknown opcodes')
    eq((s['opcode_histogram'].get('0x59'), s['opcode_histogram'].get('0x5E')), (1, 1), 'opcodes seen')


# ── frames ───────────────────────────────────────────────────────────────────

@test('frames = kSwap events; --frames N stops after the Nth swap')
def _():
    t = base_trace()
    for _i in range(3):
        t.packet(draw2()).event(0)
    p = t.save('frames')
    s, eng = summary(p)
    eq(s['frames'], 3, 'frames')
    eq([d['frame'] for d in eng.draws], [0, 1, 2], 'frame index per draw')
    eq(s['stopped_at_frame_limit'], False, 'no limit')
    s, eng = summary(p, max_frames=2)
    eq((s['frames'], s['draw_packets'], s['stopped_at_frame_limit']), (2, 2, True), 'limited')
    s, eng = summary(p, max_frames=1)
    eq((s['frames'], s['draw_packets']), (1, 1), 'limit 1')


# ── indirect buffers ─────────────────────────────────────────────────────────

def ib_trace(dispatch=None, ib_mem=True, ptr=0x1F100000):
    """A (draw A) / IB dispatch / [IB span: draw B] / wrapper End / (draw C)
    trace. ib_mem supplies the IB bytes in memory so a reader that wrongly
    executes the dispatch from memory runs draw B twice."""
    t = base_trace()
    if ib_mem:
        t.mem_read(ptr, be_bytes(draw2(count=7)))
    t.packet(draw2(count=1))
    t.packet_start(dispatch if dispatch is not None else ib_dispatch_short(ptr))
    t.ib_start(ptr)
    t.packet(draw2(count=7), base=ptr)
    t.ib_end()
    t.packet_end()
    t.packet(draw2(count=3))
    t.prim_end()
    return t


@test('INDIRECT_BUFFER short dispatch: consumed once, inner draw counted once, spans tracked')
def _():
    s, eng = summary(ib_trace().save('ib'))
    eq(s['draw_packets'], 3, 'draw packets (dispatch not re-executed from memory)')
    eq([d['count'] for d in eng.draws], [1, 7, 3], 'draw order')
    eq([d['in_ib'] for d in eng.draws], [False, True, False], 'in_ib')
    eq((s['ib_dispatch_records'], s['ib_wrapper_ends'], s['ib_spans'], s['primary_spans']),
       (1, 1, 1, 1), 'span bookkeeping')
    eq(s['packet_records'], 4, 'packet records')
    eq(s['packets_executed'], 3, 'packets executed excludes the dispatch record')
    eq(s['opcode_histogram'].get('0x3F'), 1, 'dispatch counted once in the histogram')
    eq(s['warning_counts'], {}, 'no warnings')
    eq(s['records'], {'0': 1, '1': 1, '2': 1, '3': 1, '4': 4, '5': 4, '6': 1, '10': 1}, 'record tags')


@test('INDIRECT_BUFFER_PFD full dispatch: same nesting, counted as 0x37, never re-executed')
def _():
    s, eng = summary(ib_trace(dispatch=ib_pfd(0x1F100000, 2)).save('ib_pfd'))
    eq(s['draw_packets'], 3, 'draw packets')
    eq((s['ib_dispatch_records'], s['ib_wrapper_ends']), (1, 1), 'dispatch consumed once')
    eq(s['opcode_histogram'].get('0x37'), 1, 'PFD counted')
    eq(s['opcode_histogram'].get('0x3F'), None, 'no 0x3F')


@test('trace opening inside an IB: leading IndirectBufferEnd tolerated, warned, counted')
def _():
    t = base_trace().packet(draw2(count=1)).ib_end().packet_end().packet(draw2(count=2))
    s, eng = summary(t.save('ib_leading'))
    eq(s['draw_packets'], 2, 'draws')
    eq((s['leading_ib_ends'], s['ib_wrapper_ends']), (1, 1), 'leading end + wrapper end')
    ck('leading_ib_end' in s['warning_counts'], 'warned')
    eq(s['status'], 'ok', 'status')


@test('IndirectBufferEnd after a balanced span -> exit 2')
def _():
    t = base_trace().packet_start(ib_dispatch_short(0x1F100000)).ib_start().ib_end().packet_end()
    off = t.off()
    expect_refusal(t.ib_end().save('ib_extra_end'), X.EXIT_MALFORMED, off)


@test('IndirectBufferStart while a NON-dispatch packet is pending -> exit 2')
def _():
    t = base_trace().packet_start(draw2())
    off = t.off()
    expect_refusal(t.ib_start().save('ib_bad_pending'), X.EXIT_MALFORMED, off)


@test('Xenia count==0 IB quirk (start, no end): parses, warns ib_unbalanced')
def _():
    t = base_trace().packet_start(ib_dispatch_short(0x1F100000)).ib_start().packet_end().packet(draw2())
    s, eng = summary(t.save('ib_zero'))
    eq(s['draw_packets'], 1, 'draws')
    eq(s['ib_wrapper_ends'], 1, 'wrapper end consumed')
    ck('ib_unbalanced' in s['warning_counts'], 'unterminated span warned')


# ── memory ───────────────────────────────────────────────────────────────────

@test('raw memory read applied; LOAD_ALU_CONSTANT reads 8 dwords from it; pages counted')
def _():
    vals = [fl(float(i)) for i in range(1, 9)]
    p = base_trace().mem_read(0x1F400000, be_bytes(vals)) \
        .packet(load_alu(0x1F400000, 0, 4, 8)).packet(draw2()).save('mem_raw')
    s, eng = summary(p)
    d = eng.draws[0]
    eq(list(d['vs_consts'][4:12]), vals, 'ALU constants c1..c2 from memory')
    eq(list(d['vs_consts'][0:4]), [0, 0, 0, 0], 'c0 untouched')
    eq(s['unmapped_reads'], 0, 'no unmapped reads')
    eq(s['memory_read_records'], 1, 'read records')
    eq(s['memory_read_bytes'], 32, 'read bytes')
    ck(eng.mem.has(0x1F400000) and eng.mem.has(0x1F000100), 'memory and packet pages present')
    eq(s['memory_pages'], 2, 'pages: one memory page + one packet page')


@test('raw memory record whose payload != decoded length -> exit 2; encoded past EOF -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, b'12345678', dlen=16).save('mem_len'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, b'1234', elen=4000).save('mem_elen'), X.EXIT_MALFORMED, off)


@test('unknown memory encoding -> exit 4; absurd decoded length -> exit 2')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, b'1234', enc=2).save('mem_enc2'), X.EXIT_UNSUPPORTED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, b'1234', dlen=1 << 30).save('mem_huge'), X.EXIT_MALFORMED, off)


@test('snappy memory read decoded exactly; length mismatch and corrupt payload -> exit 2')
def _():
    if cramjam is None:
        raise Skip('cramjam not installed')
    data = bytes((i * 7) & 0xFF for i in range(70000))
    comp = bytes(cramjam.snappy.compress_raw(data))
    p = base_trace().mem_read(0x1F400000, comp, enc=1, dlen=len(data)) \
        .packet(load_alu(0x1F400000 + 65536, 0, 0, 4)).packet(draw2()).save('snappy_ok')
    s, eng = summary(p)
    got, missing = eng.mem.read(0x1F400000, len(data))
    eq(missing, 0, 'no missing bytes')
    eq(got, data, 'decoded bytes equal the source')
    eq(s['memory_pages'], 3, 'two memory pages + one packet page')
    eq(list(eng.draws[0]['vs_consts'][0:4]), list(struct.unpack('>4I', data[65536:65536 + 16])), 'constants from page 2')
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, comp, enc=1, dlen=len(data) + 1).save('snappy_len'),
                   X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_read(0x1F400000, b'\xff' * 10, enc=1, dlen=100).save('snappy_bad'),
                   X.EXIT_MALFORMED, off)


@test('snappy without cramjam -> exit 3 dependency refusal at the record (never skipped)')
def _():
    t = Trace()
    off = t.off()
    p = t.mem_read(0x1F400000, b'\x04\x0c1234', enc=1, dlen=4).save('snappy_dep')
    saved = X.cramjam
    X.cramjam = None
    try:
        expect_refusal(p, X.EXIT_DEPENDENCY, off)
    finally:
        X.cramjam = saved


@test('memory WRITE records are not inputs; a read from there is counted as unmapped, never zero-filled silently')
def _():
    p = base_trace().mem_write(0x1F500000, be_bytes([1, 2, 3, 4])) \
        .packet(load_alu(0x1F500000, 0, 0, 4)).packet(draw2()).save('mem_write')
    s, eng = summary(p)
    eq(s['memory_write_records'], 1, 'write records')
    eq(s['memory_read_records'], 0, 'read records')
    eq(s['unmapped_reads'], 1, 'unmapped reads')
    eq(s['unmapped_bytes'], 16, 'unmapped bytes')
    ck('unmapped_read' in s['warning_counts'], 'warned by name')
    ck(any('LOAD_ALU_CONSTANT' in w for w in s['warnings']), 'warning names the reader')


@test('IM_LOAD from unmapped memory: counted, not hashed, warned')
def _():
    s, eng = summary(base_trace().packet(im_load(0, 0x1F600000, 6)).packet(draw2()).save('shader_unmapped'))
    eq(s['shader_loads']['vs'], 1, 'vs loads')
    eq(eng.draws[0]['vs'], None, 'hash is null')
    eq(len(eng.ucodes), 0, 'no ucode kept')
    ck('shader_unmapped' in s['warning_counts'], 'warned')


@test('EDRAM snapshot and gamma ramp: exact-size raw payloads validated and counted')
def _():
    s, eng = summary(Trace().edram(b'\0' * X.EDRAM_BYTES).gamma(0, b'\0' * X.GAMMA_BYTES).save('edram_gamma'))
    eq((s['edram_snapshots'], s['gamma_ramps']), (1, 1), 'counts')
    eq((X.EDRAM_BYTES, X.GAMMA_BYTES), (10485760, 2560), 'pinned sizes')


# ── shaders ──────────────────────────────────────────────────────────────────

@test('IM_LOAD_IMMEDIATE VS/PS: xxhash XXH3-64 of the big-endian bytes; census counts the vfetch')
def _():
    if xxhash is None:
        raise Skip('xxhash not installed')
    vs, ps = vs_simple(), [0x00000000, 0x00001000, 0x00000000]
    p = base_trace().packet(im_load_imm(0, vs)).packet(im_load_imm(1, ps)).packet(draw2()).save('imm')
    s, eng = summary(p)
    d = eng.draws[0]
    eq(d['vs'], xxhash.xxh3_64_intdigest(be_bytes(vs)), 'vs hash')
    eq(d['ps'], xxhash.xxh3_64_intdigest(be_bytes(ps)), 'ps hash')
    eq((d['vs_bytes'], d['ps_bytes']), (24, 12), 'ucode sizes')
    eq(s['shader_loads'], {'vs': 1, 'ps': 1}, 'loads')
    eq(s['distinct_shaders'], {'vs': 1, 'ps': 1, 'pairs': 1, 'unidentified_draws': 0}, 'distinct')
    vf = s['vfetch_census']
    eq((vf['vs_loads'], vf['parsed'], vf['failed'], vf['attrs'], vf['bindings']), (1, 1, 0, 1, 1), 'census')
    eq(vf['per_format'], {'k_32_32_32_FLOAT': 1}, 'format histogram')
    eq((vf['naive_divergent_loads'], vf['naive_extra'], vf['naive_missing']), (0, 0, 0), 'naive agrees here')


@test('IM_LOAD from memory hashes the same bytes as the immediate load')
def _():
    if xxhash is None:
        raise Skip('xxhash not installed')
    vs = vs_simple()
    p = base_trace().mem_read(0x1F700000, be_bytes(vs)).packet(im_load(0, 0x1F700000, len(vs))) \
        .packet(draw2()).save('im_load')
    eng, _, _ = parse(p)
    eq(eng.draws[0]['vs'], xxhash.xxh3_64_intdigest(be_bytes(vs)), 'hash')
    eq(eng.vf['attrs'], 1, 'census attr')


@test('missing xxhash: parse refuses up front (exit 3 at offset 0); hash_ucode never returns 0; --no-shader-hash gives null')
def _():
    p = base_trace().packet(im_load_imm(0, vs_simple())).packet(draw2()).save('xxhash_dep')
    empty = base_trace().packet(draw2()).save('xxhash_dep_noshader')
    saved = X.xxhash
    X.xxhash = None
    try:
        expect_refusal(p, X.EXIT_DEPENDENCY, 0)
        expect_refusal(empty, X.EXIT_DEPENDENCY, 0)
        try:
            h = X.hash_ucode(b'\0' * 12, 123)
        except X.TraceError as e:
            eq((e.code, e.offset), (X.EXIT_DEPENDENCY, 123), 'hash_ucode refusal')
        else:
            raise Fail('hash_ucode returned %r without xxhash' % (h,))
        s, eng = summary(p, hash_shaders=False)
        ck(eng.draws[0]['vs'] is None, 'hash is null (not 0) without xxhash')
        eq(s['distinct_shaders']['vs'], 0, 'no distinct hashes counted')
        eq(s['shader_loads']['vs'], 1, 'load still counted')
        eq(s['vfetch_census']['attrs'], 1, 'census still runs')
    finally:
        X.xxhash = saved


# ── constants ────────────────────────────────────────────────────────────────

@test('SET_CONSTANT ALU: VS c3 and PS c1 land in the right float files')
def _():
    p = base_trace().packet(set_const(0, 12, fl(1.0), fl(2.0), fl(3.0), fl(4.0))) \
        .packet(set_const(0, 1024 + 4, fl(9.0), fl(8.0), fl(7.0), fl(6.0))).packet(draw2()).save('alu')
    eng, _, _ = parse(p)
    d = eng.draws[0]
    eq([X.f32(v) for v in d['vs_consts'][12:16]], [1.0, 2.0, 3.0, 4.0], 'vs c3')
    eq([X.f32(v) for v in d['ps_consts'][4:8]], [9.0, 8.0, 7.0, 6.0], 'ps c1')
    eq(list(d['ps_consts'][0:4]), [0, 0, 0, 0], 'ps c0 untouched')


@test('SET_CONSTANT fetch/bool/loop/register types; unknown type warned and ignored')
def _():
    p = base_trace().packet(set_const(1, 0, 0x1F800003, (16 << 2) | 2)) \
        .packet(set_const(2, 0, 0xFFFF)).packet(set_const(3, 0, 0x12)) \
        .packet(set_const(4, 0x208, 6)).packet(set_const(5, 0, 1)).packet(draw2()).save('ctypes')
    s, eng = summary(p)
    d = eng.draws[0]
    eq(list(d['fetch'][0:2]), [0x1F800003, (16 << 2) | 2], 'fetch constant 0')
    eq(d['bools'][0], 0xFFFF, 'bool constant')
    eq(d['loops'][0], 0x12, 'loop constant')
    eq(d['kind'], 'resolve', 'REGISTERS type wrote RB_MODECONTROL')
    ck('set_constant_type' in s['warning_counts'], 'unknown constant type warned')
    v = eng.vfetch(0)
    eq((v['type'], v['addr'], v['size'], v['endian']), (3, 0x1F800000, 64, 2), 'vfetch constant decode')


@test('SET_CONSTANT2 / SET_SHADER_CONSTANTS write registers by absolute index')
def _():
    p = base_trace().packet(set_const2(0x2208, 6)).packet(t3(0x56, 0x4000, fl(1.0))).packet(draw2()).save('sc2')
    eng, _, _ = parse(p)
    eq(eng.draws[0]['kind'], 'resolve', 'SET_CONSTANT2 -> RB_MODECONTROL')
    eq(X.f32(eng.draws[0]['vs_consts'][0]), 1.0, 'SET_SHADER_CONSTANTS -> c0.x')


@test('LOAD_ALU_CONSTANT fetch type from memory; unknown type warned')
def _():
    p = base_trace().mem_read(0x1F400000, be_bytes([0x1F800003, 0x40])) \
        .packet(load_alu(0x1F400000, 1, 2, 2)).packet(load_alu(0x1F400000, 7, 0, 2)).packet(draw2()).save('load_fetch')
    s, eng = summary(p)
    eq(list(eng.draws[0]['fetch'][2:4]), [0x1F800003, 0x40], 'fetch constant 1 from memory')
    ck('load_alu_type' in s['warning_counts'], 'unknown type warned')


@test('REG_RMW: immediate and register-indirect masks (register index is 13-bit, per the pin)')
def _():
    # pm4_command_processor_implement.h L765: values[rmw_info & 0x1FFF] - a
    # register at or above 0x2000 cannot be named by REG_RMW at this pin.
    p = base_trace().packet(t0(0x0100, 0xFF)).packet(t3(0x21, 0x0100, 0x0F, 0x30)) \
        .packet(t0(0x0101, 0x03)).packet(t3(0x21, 0x0100 | (1 << 31), 0x0101, 0)) \
        .packet(t0(0x0102, 0xC0)).packet(t3(0x21, 0x0100 | (1 << 30), 0xFFFFFFFF, 0x0102)).save('rmw')
    eng, _, _ = parse(p)
    eq(eng.rd(0x0100), 0xC3, 'RMW chain: ((0xFF & 0x0F | 0x30) & reg[0x101]) | reg[0x102]')


# ── vertex fetch decode (XT1) ────────────────────────────────────────────────

@test('decode_vfetch field layout (ucode.h VertexFetchInstruction)')
def _():
    d0, d1, d2 = vfetch(dst=5, fmt=25, stride=8, offset=-4, const=3, sel=2, exp_adjust=-1, mini=1, src=7)
    op = X.decode_vfetch(d0, d1, d2, 9)
    eq((op['slot'], op['opcode'], op['src'], op['dst'], op['must_be_one']), (9, 0, 7, 5, 1), 'word 0')
    eq(op['fetch_const'], 3 * 3 + 2, 'fetch constant = index*3 + sel')
    eq((op['fmt'], op['exp_adjust'], op['mini'], op['dst_swiz']), (25, -1, 1, 0xE88), 'word 1')
    eq((op['stride'], op['offset']), (8, -4), 'word 2')


@test('XT1: the CF walk names the fetch slots; the naive triple scan also fires on an ALU slot')
def _():
    vs = cf_pair(cf(1, 1, 2, 0b0001), cf(2, 3, 1, 0)) + vfetch(fmt=57) \
        + [1 << 19, 0x1000, 0] + [0, 0, 0]
    r = X.parse_vs_vfetch(be_bytes(vs))
    ck(r['ok'] and not r['malformed'], 'walk ok')
    eq(r['exec_slots'], [(1, True), (2, False), (3, False)], 'exec slots from the sequence bits')
    eq(sorted(r['vfetch_slots']), [1], 'vertex fetch slots')
    eq(len(r['attrs']), 1, 'attrs')
    eq(r['attrs'][0]['fmt'], 57, 'attr format')
    naive = X.parse_vs_vfetch_naive(be_bytes(vs))
    eq(sorted(h['slot'] for h in naive), [1, 2], 'naive scan misclassifies slot 2')
    p = base_trace().packet(im_load_imm(0, vs)).packet(draw2()).save('xt1')
    s, eng = summary(p, hash_shaders=False)
    vf = s['vfetch_census']
    eq((vf['attrs'], vf['bindings'], vf['naive_divergent_loads'], vf['naive_extra'], vf['naive_missing']),
       (1, 1, 1, 1, 0), 'census: one divergent load, one extra naive hit')
    eq(vf['naive_extra_formats'], {'kUndefined': 1}, 'the phantom fetch has format 0')


@test('mini fetch inherits fetch constant + stride; unused-dst fetch binds nothing; must_be_one counted')
def _():
    vs = cf_pair(cf(2, 1, 4, 0b01010101)) \
        + vfetch(dst=1, fmt=57, stride=6, const=2, sel=1) \
        + vfetch(dst=2, fmt=37, stride=0, const=0, sel=0, mini=1, offset=3) \
        + vfetch(dst=3, fmt=33, stride=4, const=1, sel=0, swiz=0xFFF) \
        + vfetch(dst=4, fmt=6, stride=1, const=5, sel=0, must_be_one=0)
    r = X.parse_vs_vfetch(be_bytes(vs))
    ck(r['ok'], 'walk ok')
    eq(len(r['attrs']), 3, 'attrs: full, mini, and the must_be_one=0 one; the unused-dst one binds nothing')
    eq([(a['fetch_const'], a['stride'], a['fmt']) for a in r['attrs']],
       [(7, 6, 57), (7, 6, 37), (15, 1, 6)], 'mini inherits const 7 / stride 6')
    eq(r['attrs'][1]['offset'], 3, 'mini keeps its own offset')
    eq([(b['fetch_const'], len(b['attrs'])) for b in r['bindings']], [(7, 2), (15, 1)], 'bindings')
    eq(r['must_be_one_violations'], 1, 'must_be_one violation counted')
    eq(sorted(r['vfetch_slots']), [1, 2, 3, 4], 'all four are vertex fetch slots')


@test('texture fetch slots are not vertex fetches; a mini fetch with no prior full fetch binds nothing')
def _():
    vs = cf_pair(cf(2, 1, 2, 0b0101)) + vfetch(opcode=1) + vfetch(mini=1)
    r = X.parse_vs_vfetch(be_bytes(vs))
    ck(r['ok'], 'walk ok')
    eq(r['exec_slots'], [(1, True), (2, True)], 'both are fetch slots')
    eq(len(r['attrs']), 0, 'no vertex attrs')
    eq(sorted(r['vfetch_slots']), [2], 'only the vfetch opcode counts')


@test('exec pointing past the ucode is malformed; kExecEnd is an exec; no exec -> not ok')
def _():
    r = X.parse_vs_vfetch(be_bytes(cf_pair(cf(2, 5, 1, 1))))
    ck(not r['ok'] and r['malformed'], 'malformed')
    r = X.parse_vs_vfetch(be_bytes(cf_pair(cf(0, 0))) + be_bytes([0, 0, 0]))
    ck(not r['ok'] and not r['malformed'], 'no exec: not ok, not malformed')
    eq(X.parse_vs_vfetch(b'')['ok'], False, 'empty')
    s, eng = summary(base_trace().packet(im_load_imm(0, cf_pair(cf(2, 5, 1, 1)))).save('vs_bad'),
                     hash_shaders=False)
    eq((s['vfetch_census']['parsed'], s['vfetch_census']['failed']), (0, 1), 'census failed count')


@test('dst-swizzle usage rule and format names')
def _():
    ck(X._dst_used(0xE88) and not X._dst_used(0xFFF) and X._dst_used(0xFFB), 'used components')
    eq((X.VFMT[25], X.VFMT[57], X.VFMT[6], X.VFMT[38]),
       ('k_16_16', 'k_32_32_32_FLOAT', 'k_8_8_8_8', 'k_32_32_32_32_FLOAT'), 'names')
    ck(24 not in X.VFMT, 'no invented format 24')


@test('texture fetch constant decode (dim/size/base) - the r1-era fields, now from the repaired decoder')
def _():
    d0 = 2 | (5 << 22) | (1 << 31)
    d1 = 6 | (2 << 6) | ((0x1F800000 >> 12) << 12)
    d2 = (639) | (359 << 13)
    d5 = (1 << 9)
    t = X.decode_tfetch([d0, d1, d2, 0, 0, d5])
    eq((t['type'], t['format'], t['endianness'], t['base_address_bytes'], t['pitch'], t['pitch_pixels'], t['tiled'], t['dimension']),
       (2, 6, 2, 0x1F800000, 5, 160, 1, 1), 'fields')
    eq(X.tfetch_dims(t), (640, 360, 1), 'dims')


# ── memory model ─────────────────────────────────────────────────────────────

@test('sparse pages: a 64 KB + 1 byte read spans two pages; unmapped bytes are reported')
def _():
    m = X.Memory()
    m.write(0x1F000000, b'\xAB' * 65537)
    eq(m.page_count(), 2, 'pages')
    data, missing = m.read(0x1F00FFF0, 17)
    eq(missing, 0, 'inside')
    eq(data, b'\xAB' * 17, 'bytes')
    eq(m.read(0x1F00FFF0, 32)[1], 15, '15 bytes past the supplied 65537 are missing')
    data, missing = m.read(0x1F010000, 8)
    eq((data, missing), (b'\xAB' + b'\0' * 7, 7), 'partly unmapped read reports 7 missing bytes')
    ck(m.has(0x1F010000) and not m.has(0x1F010001) and not m.has(0x1F020000), 'byte-exact coverage')
    ck(m.has(0x1F000000, 65537) and not m.has(0x1F000000, 65538), 'range coverage')
    m.write(0x1F010001, b'\xCD' * 3)
    eq(m.uncovered(0x1F00FFFF, 0x1F010010), 12, 'adjacent write merged; 12 of 17 bytes still unsupplied')
    eq(m.read(0x1F010000, 4), (b'\xAB\xCD\xCD\xCD', 0), 'merged bytes readable')


# ── CLI / JSON ───────────────────────────────────────────────────────────────

def rich_trace(name='rich'):
    verts = [fl(1.0), fl(2.0), fl(3.0), fl(4.0), fl(5.0), fl(6.0)]
    return base_trace().mem_read(0x1F800000, be_bytes(verts)) \
        .packet(set_const(1, 0, 0x1F800003, (6 << 2) | 2)) \
        .packet(set_const(1, 6, 2 | (1 << 31), 6 | (2 << 6) | ((0x1F900000 >> 12) << 12), 639 | (359 << 13), 0, 0, 1 << 9)) \
        .packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, [0, 0x1000, 0])) \
        .packet(set_const(0, 0, fl(0.5), fl(0.25), fl(0.125), fl(1.0))) \
        .packet(draw2(count=2)).packet(t0(0x2208, 6)) \
        .packet_start(draw2()).mem_read(0x1F800000, be_bytes(verts)).packet_end().event(0).save(name)


@test('--json summary: schema, sha256, header, counts, warnings; human output preserved')
def _():
    p = rich_trace()
    out = os.path.join(TMP, 'rich.json')
    rc, so, se = run_main([p, '--json', out, '--list', '--shaders', '--resolves', '--vfetch-census',
                           '--draw', '1', '--consts', '--verts', '2', '--textures',
                           '--dump-ucode', os.path.join(TMP, 'uc'), '--dump-verts', os.path.join(TMP, 'v.bin')])
    eq(rc, 0, 'exit code; stderr=%r' % se)
    j = json.load(open(out, encoding='utf-8'))
    eq(j['schema'], 'xtr_draws-summary/7', 'schema')
    eq(j['input_sha256'], hashlib.sha256(open(p, 'rb').read()).hexdigest(), 'input sha256')
    eq(j['input_bytes'], os.path.getsize(p), 'input bytes')
    eq((j['xenia_build_sha'], j['title_id'], j['trace_version']), (XENIA_SHA.decode(), '454107ED', 1), 'header')
    eq((j['frames'], j['draw_packets'], j['draws'], j['resolves'], j['status'], j['refusal']),
       (1, 2, 1, 1, 'ok', None), 'counts')
    eq(j['memory_pages'], 2, 'pages')
    eq(j['shader_loads'], {'vs': 1, 'ps': 1}, 'loads')
    eq(j['distinct_shaders'], {'vs': 1, 'ps': 1, 'pairs': 1, 'unidentified_draws': 0}, 'identified pair')
    eq(len(j['resolve_details']), 1, 'resolve details')
    rd = j['resolve_details'][0]
    eq((rd['draw_ord'], rd['source']['kind'], rd['source']['index'], rd['source']['register']),
       (2, 'color', 0, 0x2001), 'resolve detail source')
    for k in ('copy_control', 'copy', 'dest_base', 'dest_pitch_decoded', 'dest_info', 'dest',
              'surface_info', 'rt_info', 'depth_info', 'source', 'get_resolve_info_inputs'):
        ck(k in rd, 'resolve detail key %s' % k)
    gi = rd['get_resolve_info_inputs']
    eq(gi['status'], 'complete', 'rich trace: vf0 payload was supplied before the draw')
    eq(gi['vertex_payload']['dwords_be'], [fl(1.0), fl(2.0), fl(3.0), fl(4.0), fl(5.0), fl(6.0)], 'rich trace payload')
    eq(j['resolve_inputs'], dict(complete=1, rejected=0, incomplete=0, total=1, distinct_vertex_payloads=1,
                                 distinct_vertex_addresses=1, not_in_trace=list(X.RESOLVE_INPUTS_NOT_IN_TRACE)),
       'resolve_inputs summary')
    eq(j['vfetch_census']['attrs'], 1, 'census')
    for k in ('unknown_opcodes', 'opcode_histogram', 'warnings', 'warning_counts', 'records',
              'packet_records', 'packets_executed', 'ib_dispatch_records', 'memory_read_records',
              'memory_read_bytes', 'unmapped_reads', 'screen_extent_events', 'zero_index_draws',
              'xenia_pin', 'mcla_pin', 'tool_version'):
        ck(k in j, 'json key %s present' % k)
    ck('#1' in so and 'resolve' in so and 'v0:' in so and 't0 ' in so and 'distinct (vs,ps) pairs' in so
       and 'vs=' in so and 'EXTRA=[]' in so, 'human views present: %r' % so[:400])
    eq(open(os.path.join(TMP, 'uc_vs.bin'), 'rb').read(), be_bytes(vs_simple()), 'dumped VS ucode')
    eq(open(os.path.join(TMP, 'v.bin'), 'rb').read(), be_bytes([fl(1.0), fl(2.0), fl(3.0), fl(4.0), fl(5.0), fl(6.0)]), 'dumped verts')


@test('--json on a refused trace: exit code propagated, JSON status refused with offset')
def _():
    t = Trace()
    off = t.off()
    p = t.tag(12).save('refused_json')
    out = os.path.join(TMP, 'refused.json')
    rc, so, se = run_main([p, '--json', out])
    eq(rc, X.EXIT_UNSUPPORTED, 'exit code')
    ck('REFUSED' in se and str(off) in se, 'stderr names the offset: %r' % se)
    j = json.load(open(out, encoding='utf-8'))
    eq((j['status'], j['refusal']['code'], j['refusal']['offset']), ('refused', X.EXIT_UNSUPPORTED, off), 'json refusal')
    eq(j['input_sha256'], hashlib.sha256(open(p, 'rb').read()).hexdigest(), 'sha still recorded')


@test('CLI: missing file -> 1; draw ordinal out of range -> 1; --frames honoured; --no-shader-hash')
def _():
    p = rich_trace('cli')
    eq(run_main([os.path.join(TMP, 'nope.xtr')])[0], X.EXIT_USAGE, 'missing file')
    eq(run_main([p, '--draw', '99'])[0], X.EXIT_USAGE, 'draw out of range')
    out = os.path.join(TMP, 'cli.json')
    rc, so, se = run_main([p, '--frames', '1', '--json', out, '--no-shader-hash'])
    eq(rc, 0, 'exit')
    j = json.load(open(out, encoding='utf-8'))
    eq((j['frames'], j['stopped_at_frame_limit']), (1, True), 'frame limit')
    eq(j['distinct_shaders'], {'vs': 0, 'ps': 0, 'pairs': 0, 'unidentified_draws': 2},
       'no-hash: no distinct shaders, NO pair, every draw unidentified')


# ── r1: every supplied payload is validated (memory writes, EDRAM, gamma) ────

def snappy(data):
    if cramjam is None:
        raise Skip('cramjam not installed')
    return bytes(cramjam.snappy.compress_raw(data))


@test('memory WRITE payloads are validated then discarded: raw and snappy accepted')
def _():
    data = be_bytes([1, 2, 3, 4])
    p = base_trace().mem_write(0x1F500000, data).mem_write(0x1F510000, snappy(data), enc=1, dlen=16) \
        .packet(load_alu(0x1F510000, 0, 0, 4)).packet(draw2()).save('mw_ok')
    s, eng = summary(p)
    eq((s['memory_write_records'], s['memory_write_bytes']), (2, 32), 'write records counted')
    eq(s['unmapped_reads'], 1, 'a validated write is still NOT input memory')
    ck(not eng.mem.has(0x1F500000) and not eng.mem.has(0x1F510000), 'neither write mapped')


@test('memory WRITE malformed classes: raw length mismatch -> 2, unknown encoding -> 4, corrupt snappy -> 2, no cramjam -> 3')
def _():
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_write(0x1F500000, b'12345678', dlen=16).save('mw_len'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_write(0x1F500000, b'1234', enc=2).save('mw_enc'), X.EXIT_UNSUPPORTED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_write(0x1F500000, b'\xff' * 10, enc=1, dlen=100).save('mw_corrupt'),
                   X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.mem_write(0x1F500000, snappy(b'\0' * 64), enc=1, dlen=65).save('mw_snappy_len'),
                   X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    p = t.mem_write(0x1F500000, b'\x04\x0c1234', enc=1, dlen=4).save('mw_dep')
    saved = X.cramjam
    X.cramjam = None
    try:
        expect_refusal(p, X.EXIT_DEPENDENCY, off)
    finally:
        X.cramjam = saved


@test('EDRAM snapshot must decode to exactly kEdramSizeBytes: snappy ok; undersized raw, wrong-size snappy, corrupt, unknown enc, no cramjam refused')
def _():
    full = bytes(X.EDRAM_BYTES)
    s, eng = summary(Trace().edram(snappy(full), enc=1).save('edram_snappy'))
    eq(s['edram_snapshots'], 1, 'snappy snapshot accepted')
    t = Trace()
    off = t.off()
    expect_refusal(t.edram(b'\0' * 16).save('edram_short'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.edram(b'\0' * (X.EDRAM_BYTES + 4)).save('edram_long'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.edram(snappy(b'\0' * 100), enc=1).save('edram_snappy_short'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.edram(b'\xff' * 10, enc=1).save('edram_corrupt'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.edram(b'\0' * 4, enc=2).save('edram_enc'), X.EXIT_UNSUPPORTED, off)
    t = Trace()
    off = t.off()
    p = t.edram(b'\x04\x0c1234', enc=1).save('edram_dep')
    saved = X.cramjam
    X.cramjam = None
    try:
        expect_refusal(p, X.EXIT_DEPENDENCY, off)
    finally:
        X.cramjam = saved


@test('gamma ramp must decode to exactly (256+3*128)*4 bytes: snappy ok; undersized raw, wrong-size snappy, corrupt, unknown enc, no cramjam refused')
def _():
    full = bytes(range(256)) * (X.GAMMA_BYTES // 256)
    s, eng = summary(Trace().gamma(2, snappy(full), enc=1).save('gamma_snappy'))
    eq(s['gamma_ramps'], 1, 'snappy ramp accepted')
    t = Trace()
    off = t.off()
    expect_refusal(t.gamma(0, b'\0' * 8).save('gamma_short'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.gamma(0, snappy(b'\0' * 2556), enc=1).save('gamma_snappy_short'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.gamma(0, b'\xff' * 10, enc=1).save('gamma_corrupt'), X.EXIT_MALFORMED, off)
    t = Trace()
    off = t.off()
    expect_refusal(t.gamma(0, b'\0' * 4, enc=2).save('gamma_enc'), X.EXIT_UNSUPPORTED, off)
    t = Trace()
    off = t.off()
    p = t.gamma(0, b'\x04\x0c1234', enc=1).save('gamma_dep')
    saved = X.cramjam
    X.cramjam = None
    try:
        expect_refusal(p, X.EXIT_DEPENDENCY, off)
    finally:
        X.cramjam = saved


# ── r1: the resolve SOURCE is the selected register, not RT0 ─────────────────

RT_VALUES = [0x5A0 | (0 << 16) | (0 << 20),          # RT0 k_8_8_8_8, bias 0
             0x658 | (4 << 16) | (0x3E << 20),       # RT1 k_16_16, bias -2
             0x700 | (6 << 16) | (0x01 << 20),       # RT2 k_16_16_FLOAT, bias +1
             0x7FF | (15 << 16) | (0x20 << 20)]      # RT3 k_32_32_FLOAT, bias -32
DEPTH_VALUE = 0x2D0 | (1 << 16)                       # kD24FS8


def source_trace(selects, depth=DEPTH_VALUE, name='src'):
    t = Trace().packet(t0(0x2001, RT_VALUES[0])).packet(t0(0x2003, RT_VALUES[1])) \
        .packet(t0(0x2004, RT_VALUES[2])).packet(t0(0x2005, RT_VALUES[3])) \
        .packet(t0(0x2002, depth)).packet(t0(0x2208, 6))
    for sel in selects:
        t.packet(t0(0x2318, sel | (1 << 20))).packet(draw2())
    return t.save(name)


@test('resolve source 0..3 selects RB_COLOR_INFO / RB_COLOR1_INFO / RB_COLOR2_INFO / RB_COLOR3_INFO with the signed exponent bias')
def _():
    eng, _, _ = parse(source_trace([0, 1, 2, 3]))
    eq(len(eng.resolves), 4, 'four resolves')
    want = [(0, 0x2001, 'RB_COLOR_INFO', 0x5A0, 0, 'k_8_8_8_8', 0),
            (1, 0x2003, 'RB_COLOR1_INFO', 0x658, 4, 'k_16_16', -2),
            (2, 0x2004, 'RB_COLOR2_INFO', 0x700, 6, 'k_16_16_FLOAT', 1),
            (3, 0x2005, 'RB_COLOR3_INFO', 0x7FF, 15, 'k_32_32_FLOAT', -32)]
    for r, w in zip(eng.resolves, want):
        sc = r['source']
        eq(sc['kind'], 'color', 'kind')
        eq((sc['index'], sc['register'], sc['register_name'], sc['base'], sc['fmt'], sc['fmt_name'],
            sc['exp_bias']), w, 'source %d' % w[0])
        eq(sc['raw'], RT_VALUES[w[0]], 'raw register value')
        eq(r['rt_info'], RT_VALUES, 'all four RT registers preserved raw')
        eq(r['depth_info'], DEPTH_VALUE, 'depth register preserved raw')


@test('resolve source 4..7 selects RB_DEPTH_INFO (12-bit base, format bit 16), no exponent')
def _():
    eng, _, _ = parse(source_trace([4, 7], name='src_depth'))
    for r, sel in zip(eng.resolves, (4, 7)):
        sc = r['source']
        eq((sc['kind'], sc['index'], sc['register'], sc['register_name'], sc['raw'], sc['base'],
            sc['fmt'], sc['fmt_name'], sc['exp_bias']),
           ('depth', sel, 0x2002, 'RB_DEPTH_INFO', DEPTH_VALUE, 0x2D0, 1, 'kD24FS8', None), 'depth source %d' % sel)
    eng, _, _ = parse(source_trace([5], depth=0x123, name='src_depth0'))
    eq((eng.resolves[0]['source']['fmt'], eng.resolves[0]['source']['fmt_name'], eng.resolves[0]['source']['base']),
       (0, 'kD24S8', 0x123), 'kD24S8')


@test('decode_color_info / decode_depth_info / resolve_source_label field layout')
def _():
    c = X.decode_color_info(0x658 | (4 << 16) | (0x3F << 20))
    eq((c['base'], c['fmt'], c['fmt_name'], c['exp_bias']), (0x658, 4, 'k_16_16', -1), 'color info')
    eq(X.decode_color_info(0xFFF | (0x1F << 20))['exp_bias'], 31, 'max positive bias')
    d = X.decode_depth_info(0x102D0)
    eq((d['base'], d['fmt'], d['fmt_name']), (0x2D0, 1, 'kD24FS8'), 'depth info')
    regs = {0x2003: RT_VALUES[1], 0x2002: DEPTH_VALUE}
    sc = X.resolve_source(1, lambda i: regs.get(i, 0))
    eq(X.resolve_source_label(sc), 'COLOR1 RB_COLOR1_INFO=03E40658 base=658 fmt=k_16_16(4) expBias=-2', 'color label')
    sc = X.resolve_source(6, lambda i: regs.get(i, 0))
    eq(X.resolve_source_label(sc), 'DEPTH(src6) RB_DEPTH_INFO=000102D0 base=2D0 fmt=kD24FS8(1)', 'depth label')


@test('--resolves and resolve_details name the selected source unambiguously')
def _():
    p = source_trace([1, 4], name='src_view')
    out = os.path.join(TMP, 'src_view.json')
    rc, so, se = run_main([p, '--resolves', '--json', out])
    eq(rc, 0, 'exit')
    ck('source=[COLOR1 RB_COLOR1_INFO=03E40658 base=658 fmt=k_16_16(4) expBias=-2]' in so, 'COLOR1 line: %r' % so[-600:])
    ck('source=[DEPTH(src4) RB_DEPTH_INFO=000102D0 base=2D0 fmt=kD24FS8(1)]' in so, 'DEPTH line')
    ck('srcRT=' not in so, 'the ambiguous RT0 label is gone')
    j = json.load(open(out, encoding='utf-8'))
    eq([(d['source']['kind'], d['source']['index'], d['source']['base']) for d in j['resolve_details']],
       [('color', 1, 0x658), ('depth', 4, 0x2D0)], 'json sources')
    eq(j['resolve_details'][0]['rt_info'], RT_VALUES, 'json raw RT registers')
    json.dumps(j)


# ── r1: an unhashed shader is not an identity ────────────────────────────────

@test('unidentified draws: no hash on either side -> not a pair; counted separately in JSON and the --shaders view')
def _():
    if xxhash is None:
        raise Skip('xxhash not installed')
    vs = vs_simple()
    p = base_trace().packet(im_load_imm(0, vs)).packet(draw2()) \
        .packet(im_load_imm(1, [0, 0x1000, 0])).packet(draw2()).packet(draw2()).save('unident')
    s, eng = summary(p)
    eq(s['distinct_shaders'], {'vs': 1, 'ps': 1, 'pairs': 1, 'unidentified_draws': 1},
       'first draw has no PS: unidentified; the other two share one pair')
    s, eng = summary(p, hash_shaders=False)
    eq(s['distinct_shaders'], {'vs': 0, 'ps': 0, 'pairs': 0, 'unidentified_draws': 3}, 'no-hash: zero pairs')
    rc, so, se = run_main([p, '--shaders'])
    ck('1 distinct (vs,ps) pairs among identified draws; 1 unidentified draw(s)' in so, 'human view: %r' % so[-400:])
    rc, so, se = run_main([p, '--shaders', '--no-shader-hash'])
    ck('0 distinct (vs,ps) pairs among identified draws; 3 unidentified draw(s)' in so, 'human view no-hash: %r' % so[-400:])
    ck('unidentifiedDraws=3' in so, 'summary line')


# ── TOOL-2A: lossless GetResolveInfo inputs (schema /3) ──────────────────────

VERTS_A = [fl(0.0), fl(0.0), fl(640.0), fl(0.0), fl(640.0), fl(360.0)]
VERTS_B = [fl(16.0), fl(8.0), fl(320.0), fl(8.0), fl(320.0), fl(180.0)]
INPUT_REG_VALUES = {
    'PA_SU_VTX_CNTL': (0x2302, 0x00000100), 'PA_SU_SC_MODE_CNTL': (0x2205, 0x00010001),
    'PA_SC_WINDOW_OFFSET': (0x2080, 0x00040008), 'PA_SC_WINDOW_SCISSOR_TL': (0x2081, 0x80000000),
    'PA_SC_WINDOW_SCISSOR_BR': (0x2082, 0x02D00500), 'PA_SC_SCREEN_SCISSOR_TL': (0x200E, 0x00010002),
    'PA_SC_SCREEN_SCISSOR_BR': (0x200F, 0x1FFF1FFF), 'RB_DEPTH_CLEAR': (0x231D, 0x3F800000),
    'RB_COLOR_CLEAR': (0x231E, 0xFF00FF00), 'RB_COLOR_CLEAR_LO': (0x231F, 0x12345678),
}
VF0_ADDR = 0x1F800100


def vf0(addr_bytes, size_dw=6, endian=2, ftype=3):
    """xenos.h L1158-L1172: word0 = type | address_dwords << 2; word1 = endian | size_dwords << 2."""
    return set_const(1, 0, ((addr_bytes >> 2) << 2) | ftype, (size_dw << 2) | endian)


def inputs_trace(name, resolves, regs=INPUT_REG_VALUES):
    """A kCopy-mode trace whose resolves are given as
    (payload_bytes_or_None, placement, vf0_kwargs): placement 'inside' puts
    the payload MemoryRead between the draw's PacketStart and PacketEnd (as
    Xenia's writer does at draw_util.cc L959), 'after' puts it after
    PacketEnd, 'none' omits it, 'inside_split' records it as two reads
    (8 + 16 bytes), 'inside_overlap' records a decoy 24-byte read first and
    the real payload second, 'inside_unrelated' records 24 bytes at
    addr+0x100 instead, 'inside_write' records a same-range MemoryWrite.
    Returns (path, [PacketStart record offsets])."""
    tr = Trace().packet(t0(0x2208, 6)).packet(t0(0x2318, 1 << 20))
    for _name, (reg, val) in regs.items():
        tr.packet(t0(reg, val))
    offsets = []
    for payload, place, kw in resolves:
        tr.packet(vf0(kw.get('addr', VF0_ADDR), kw.get('size', 6), kw.get('endian', 2), kw.get('type', 3)))
        offsets.append(tr.off())
        tr.packet_start(draw2())
        addr = kw.get('addr', VF0_ADDR)
        if payload is not None:
            if place == 'inside':
                tr.mem_read(addr, payload)
            elif place == 'inside_split':
                tr.mem_read(addr, payload[:8]).mem_read(addr + 8, payload[8:])
            elif place == 'inside_overlap':
                tr.mem_read(addr, be_bytes(VERTS_B)).mem_read(addr, payload)
            elif place == 'inside_unrelated':
                tr.mem_read(addr + 0x100, payload)
            elif place == 'inside_write':
                tr.mem_write(addr, payload)
        tr.packet_end()
        if payload is not None and place == 'after':
            tr.mem_read(kw.get('addr', VF0_ADDR), payload)
    return tr.save(name), offsets


def projection2(row):
    """The schema-/2 projection of a resolve_details row (what RC-1 compared)."""
    r = dict(row)
    for k in ('draw_ord', 'frame', 'get_resolve_info_inputs', 'guest_destination_range'):
        r.pop(k, None)
    return json.dumps(r, sort_keys=True)


@test('T2A3 exact packet-end timing: the payload MemoryRead between PacketStart and PacketEnd is captured exactly, with the PacketStart record offset as provenance')
def _():
    p, offs = inputs_trace('t2a_timing', [(be_bytes(VERTS_A), 'inside', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq(gi['status'], 'complete', 'status')
    eq(gi['draw_packet_record_offset'], offs[0], 'provenance = the draw PacketStart record offset')
    eq(eng.resolves[0]['draw_ord'], eng.draws[0]['ord'], 'row is the draw')
    eq(eng.draws[0]['record_offset'], offs[0], 'draw record offset agrees')
    vp = gi['vertex_payload']
    eq((vp['status'], vp['missing_bytes']), ('complete', 0), 'payload status')
    eq(vp['dwords_be'], VERTS_A, 'six big-endian dwords, exact')
    eq(vp['bytes_hex'], be_bytes(VERTS_A).hex().upper(), '24 bytes, exact')
    eq(s['resolve_inputs'], dict(complete=1, rejected=0, incomplete=0, total=1, distinct_vertex_payloads=1,
                                 distinct_vertex_addresses=1, not_in_trace=list(X.RESOLVE_INPUTS_NOT_IN_TRACE)),
       'summary')
    eq(s['unmapped_reads'], 0, 'no unmapped read')
    eq(gi['rejection'], None, 'no rejection')


@test('payload arriving only AFTER PacketEnd is not visible to the draw: INCOMPLETE, missing 24, no bytes, warned')
def _():
    p, offs = inputs_trace('t2a_late', [(be_bytes(VERTS_A), 'after', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq(gi['status'], 'incomplete', 'status')
    vp = gi['vertex_payload']
    eq((vp['status'], vp['bytes_hex'], vp['dwords_be'], vp['missing_bytes']), ('incomplete', None, None, 24), 'payload')
    eq(s['resolve_inputs']['incomplete'], 1, 'summary incomplete')
    eq(s['unmapped_reads'], 1, 'the read of unsupplied bytes is counted')
    ck(any('resolve vertex fetch 0' in w for w in s['warnings']), 'warned by name')


@test('missing payload and partial payload (16 of 24 bytes) are INCOMPLETE with exact missing counts')
def _():
    p, offs = inputs_trace('t2a_none', [(None, 'none', {})])
    s, eng = summary(p)
    eq(eng.resolves[0]['get_resolve_info_inputs']['vertex_payload']['missing_bytes'], 24, 'all 24 missing')
    p, offs = inputs_trace('t2a_partial', [(be_bytes(VERTS_A)[:16], 'inside', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['missing_bytes'], gi['vertex_payload']['bytes_hex']),
       ('incomplete', 8, None), 'partial payload never emitted, 8 bytes missing')


@test('T2A2/T2A3: distinct per-draw payloads at a REUSED address are captured per resolve; /2 projections equal, /3 rows differ')
def _():
    p, offs = inputs_trace('t2a_reuse', [(be_bytes(VERTS_A), 'inside', {}), (be_bytes(VERTS_B), 'inside', {})])
    s, eng = summary(p)
    a, b = eng.resolves
    eq(a['get_resolve_info_inputs']['vertex_payload']['dwords_be'], VERTS_A, 'first draw sees payload A')
    eq(b['get_resolve_info_inputs']['vertex_payload']['dwords_be'], VERTS_B, 'second draw sees payload B')
    eq(a['get_resolve_info_inputs']['vertex_fetch_0']['address_bytes'],
       b['get_resolve_info_inputs']['vertex_fetch_0']['address_bytes'], 'same vf0 address')
    ra, rb = X.resolve_detail(a), X.resolve_detail(b)
    eq(projection2(ra), projection2(rb), 'the schema-/2 projection cannot tell them apart')
    ck(json.dumps(ra['get_resolve_info_inputs'], sort_keys=True) != json.dumps(rb['get_resolve_info_inputs'], sort_keys=True),
       'schema /3 distinguishes them')
    eq((ra['dest_base'], ra['dest_pitch_decoded']), (rb['dest_base'], rb['dest_pitch_decoded']),
       'raw base + declared pitch/height are identical: not a sufficient join key')
    eq(s['resolve_inputs'], dict(complete=2, rejected=0, incomplete=0, total=2, distinct_vertex_payloads=2,
                                 distinct_vertex_addresses=1, not_in_trace=list(X.RESOLVE_INPUTS_NOT_IN_TRACE)),
       'summary')
    eq((a['get_resolve_info_inputs']['draw_packet_record_offset'], b['get_resolve_info_inputs']['draw_packet_record_offset']),
       tuple(offs), 'per-row provenance')


@test('T2A1: identical /2 projections with different scissor/window inputs are distinguished only by /3')
def _():
    regs2 = dict(INPUT_REG_VALUES)
    regs2['PA_SC_WINDOW_SCISSOR_BR'] = (0x2082, 0x01680280)
    pa, _ = inputs_trace('t2a_scissor_a', [(be_bytes(VERTS_A), 'inside', {})])
    pb, _ = inputs_trace('t2a_scissor_b', [(be_bytes(VERTS_A), 'inside', {})], regs=regs2)
    ea, _, _ = parse(pa)
    eb, _, _ = parse(pb)
    ra, rb = X.resolve_detail(ea.resolves[0]), X.resolve_detail(eb.resolves[0])
    eq(projection2(ra), projection2(rb), '/2 projections identical')
    eq(ra['get_resolve_info_inputs']['registers']['PA_SC_WINDOW_SCISSOR_BR'], 0x02D00500, 'A scissor')
    eq(rb['get_resolve_info_inputs']['registers']['PA_SC_WINDOW_SCISSOR_BR'], 0x01680280, 'B scissor')
    ck(ra['get_resolve_info_inputs'] != rb['get_resolve_info_inputs'], '/3 differs')


@test('every new raw register is captured at the draw with its own value, and a later write does not leak back')
def _():
    p, offs = inputs_trace('t2a_regs', [(be_bytes(VERTS_A), 'inside', {})])
    s, eng = summary(p)
    regs = eng.resolves[0]['get_resolve_info_inputs']['registers']
    eq(sorted(regs), sorted(INPUT_REG_VALUES), 'exactly the ten registers')
    for name, (reg, val) in INPUT_REG_VALUES.items():
        eq(regs[name], val, 'register %s (0x%04X)' % (name, reg))
    # two resolves, registers changed between them: each row keeps its own values
    tr = Trace().packet(t0(0x2208, 6)).packet(t0(0x2318, 1 << 20)).packet(vf0(VF0_ADDR))
    for i in range(2):
        for name, (reg, val) in INPUT_REG_VALUES.items():
            tr.packet(t0(reg, val + i))
        tr.packet_start(draw2()).mem_read(VF0_ADDR, be_bytes(VERTS_A)).packet_end()
    eng, _, _ = parse(tr.save('t2a_regs2'))
    for i, r in enumerate(eng.resolves):
        for name, (reg, val) in INPUT_REG_VALUES.items():
            eq(r['get_resolve_info_inputs']['registers'][name], val + i, 'row %d register %s' % (i, name))


@test('vertex fetch 0 decode: raw dwords, type, address (dwords and bytes), size (dwords and bytes), endian')
def _():
    p, offs = inputs_trace('t2a_vf0', [(be_bytes(VERTS_A), 'inside', dict(addr=0x1F80_0104, endian=1))])
    eng, _, _ = parse(p)
    vf = eng.resolves[0]['get_resolve_info_inputs']['vertex_fetch_0']
    eq(vf['raw'], [((0x1F800104 >> 2) << 2) | 3, (6 << 2) | 1], 'raw dwords')
    eq((vf['type'], vf['type_name'], vf['address_dwords'], vf['address_bytes'], vf['size_dwords'], vf['size_bytes'], vf['endian']),
       (3, 'kVertex', 0x1F800104 >> 2, 0x1F800104, 6, 24, 1), 'decoded fields')
    eq(eng.resolves[0]['get_resolve_info_inputs']['vertex_payload']['dwords_be'], VERTS_A, 'payload read at the byte address')


@test('Xenia-invalid fetch TYPE (kTexture / kInvalidVertex) -> named GetResolveInfo rejection; no payload read, no fake zeros')
def _():
    # NO payload is supplied at a fresh address: a reader that still read the 24
    # bytes for a rejected fetch would show up as an unmapped read below.
    for ftype, tname in ((2, 'kTexture'), (1, 'kInvalidVertex')):
        p, offs = inputs_trace('t2a_type%d' % ftype, [(None, 'none', dict(type=ftype, addr=0x1F800300))])
        s, eng = summary(p)
        gi = eng.resolves[0]['get_resolve_info_inputs']
        eq(gi['status'], 'rejected', 'status for type %d' % ftype)
        ck('bad type' in gi['rejection'] and 'L954-L958' in gi['rejection'], 'named rejection: %r' % gi['rejection'])
        eq(gi['vertex_fetch_0']['type_name'], tname, 'type name')
        eq(gi['vertex_payload'], dict(status='not_applicable', bytes_hex=None, dwords_be=None, missing_bytes=None), 'no payload')
        eq(s['resolve_inputs']['rejected'], 1, 'summary rejected')
        eq(s['unmapped_reads'], 0, 'no read attempted')
        ck(not any('resolve vertex fetch 0' in w for w in s['warnings']), 'no payload warning for a rejected fetch')


@test('Xenia-invalid fetch SIZE (4 dwords) -> rejected size; bad type AND size are both named')
def _():
    p, offs = inputs_trace('t2a_size', [(None, 'none', dict(size=4, addr=0x1F800300))])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq(gi['status'], 'rejected', 'status')
    eq(s['unmapped_reads'], 0, 'no read attempted for a rejected size')
    ck('bad size' in gi['rejection'] and 'type' not in gi['rejection'].split('bad ')[1], 'only size named: %r' % gi['rejection'])
    eq(gi['vertex_fetch_0']['size_dwords'], 4, 'size decoded')
    p, offs = inputs_trace('t2a_both', [(be_bytes(VERTS_A), 'inside', dict(size=4, type=2))])
    eng, _, _ = parse(p)
    ck('bad type and size' in eng.resolves[0]['get_resolve_info_inputs']['rejection'], 'both named')


@test('conservation: complete + rejected + incomplete == resolves; non-resolve draws carry no inputs and read no payload')
def _():
    # the unavailable case uses a FRESH address: memory still holds the first payload at VF0_ADDR
    p, offs = inputs_trace('t2a_mix', [(be_bytes(VERTS_A), 'inside', {}), (be_bytes(VERTS_A), 'inside', dict(type=2)),
                                       (None, 'none', dict(addr=0x1F800200))])
    s, eng = summary(p)
    eq(s['resolve_inputs']['total'], s['resolves'], 'total equals resolves')
    eq((s['resolve_inputs']['complete'], s['resolve_inputs']['rejected'], s['resolve_inputs']['incomplete']), (1, 1, 1), 'mix')
    eq([r['get_resolve_info_inputs']['status'] for r in eng.resolves], ['complete', 'rejected', 'incomplete'], 'per row')
    eq(s['unmapped_reads'], 1, 'only the incomplete row read unsupplied bytes')
    # a mode-4 draw with the same vf0 and no payload: no inputs object, no read
    p2 = base_trace().packet(vf0(VF0_ADDR)).packet(draw2()).save('t2a_plain_draw')
    s2, eng2 = summary(p2)
    eq((s2['resolves'], s2['resolve_inputs']['total'], s2['unmapped_reads']), (0, 0, 0), 'plain draw untouched')
    ck('get_resolve_info_inputs' not in eng2.draws[0], 'draw rows carry no inputs object')


@test('schema /3 wiring: get_resolve_info_inputs key set, not_in_trace list exact, JSON serializable, --resolves names the status')
def _():
    p, offs = inputs_trace('t2a_json', [(be_bytes(VERTS_A), 'inside', {}), (be_bytes(VERTS_A), 'inside', dict(size=2)),
                                        (None, 'none', dict(addr=0x1F800200))])
    out = os.path.join(TMP, 't2a.json')
    rc, so, se = run_main([p, '--json', out, '--resolves'])
    eq(rc, 0, 'exit')
    j = json.load(open(out, encoding='utf-8'))
    eq(j['schema'], 'xtr_draws-summary/7', 'schema')
    for row in j['resolve_details']:
        gi = row['get_resolve_info_inputs']
        eq(sorted(gi), ['draw_packet_record_offset', 'not_in_trace', 'registers', 'rejection', 'status',
                        'vertex_fetch_0', 'vertex_payload'], 'inputs key set')
        eq(sorted(gi['vertex_fetch_0']), ['address_bytes', 'address_dwords', 'endian', 'raw', 'size_bytes',
                                         'size_dwords', 'type', 'type_name'], 'vf0 key set')
        eq(gi['not_in_trace'], ['draw_resolution_scale_x', 'draw_resolution_scale_y',
                                'resolve_resolution_scale_fill_half_pixel_offset', 'half_pixel_offset',
                                'fixed_rg16_truncated_to_minus_1_to_1', 'fixed_rgba16_truncated_to_minus_1_to_1'],
           'host parameters absent from .xtr are named, not invented')
    eq([r['get_resolve_info_inputs']['status'] for r in j['resolve_details']], ['complete', 'rejected', 'incomplete'], 'statuses')
    eq(j['resolve_inputs']['not_in_trace'], j['resolve_details'][0]['get_resolve_info_inputs']['not_in_trace'], 'summary list')
    json.dumps(j)
    ck('inputs=[complete vf0 type=3 addr=1F800100 size=6dw endian=2 verts=' in so, 'complete line: %r' % so[-900:])
    ck('inputs=[REJECTED vf0 type=kVertex(3) size=2dw' in so, 'rejected line')
    ck('inputs=[INCOMPLETE vf0 type=3 addr=1F800200 size=6dw endian=2 missing=24 bytes]' in so, 'incomplete line: %r' % so[-700:])
    ck('@record%d' % offs[0] in so, 'provenance printed')
    ck('resolveIn: complete=1 rejected=1 incomplete=1 of 3' in so, 'summary line')


# ── TOOL-2A-r1: the payload proof is packet-local ─────────────────────────────

@test('r1: complete A, then the SAME address with no current read -> INCOMPLETE missing 24 (the trace-wide image never satisfies the proof)')
def _():
    p, offs = inputs_trace('r1_reuse_missing', [(be_bytes(VERTS_A), 'inside', {}), (None, 'none', {})])
    s, eng = summary(p)
    a, b = (r['get_resolve_info_inputs'] for r in eng.resolves)
    eq((a['status'], a['vertex_payload']['dwords_be']), ('complete', VERTS_A), 'row 1 complete')
    eq((b['status'], b['vertex_payload']['status'], b['vertex_payload']['missing_bytes'],
        b['vertex_payload']['bytes_hex'], b['vertex_payload']['dwords_be']),
       ('incomplete', 'incomplete', 24, None, None), 'row 2 incomplete, no bytes')
    eq((s['resolve_inputs']['complete'], s['resolve_inputs']['incomplete']), (1, 1), 'summary')
    eq(s['unmapped_reads'], 0, 'the bytes exist in the trace-wide image: not an unmapped read')
    ck('resolve_payload_not_in_packet' in s['warning_counts'], 'warned by name')


@test('r1: complete A, then only 16 current bytes at the same address -> INCOMPLETE missing 8, no mixed payload')
def _():
    p, offs = inputs_trace('r1_reuse_partial', [(be_bytes(VERTS_A), 'inside', {}), (be_bytes(VERTS_B)[:16], 'inside', {})])
    s, eng = summary(p)
    b = eng.resolves[1]['get_resolve_info_inputs']
    eq((b['status'], b['vertex_payload']['missing_bytes'], b['vertex_payload']['bytes_hex'], b['vertex_payload']['dwords_be']),
       ('incomplete', 8, None, None), 'row 2: 8 bytes missing, nothing emitted')
    eq(s['resolve_inputs']['incomplete'], 1, 'summary')


@test('r1: a 24-byte payload split across two current reads (8 + 16) -> complete and exact')
def _():
    p, offs = inputs_trace('r1_split', [(be_bytes(VERTS_A), 'inside_split', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['missing_bytes'], gi['vertex_payload']['dwords_be']),
       ('complete', 0, VERTS_A), 'split reads joined exactly')
    eq(gi['vertex_payload']['bytes_hex'], be_bytes(VERTS_A).hex().upper(), 'bytes exact')


@test('r1: overlapping current reads -> the later record wins byte for byte; complete')
def _():
    p, offs = inputs_trace('r1_overlap', [(be_bytes(VERTS_A), 'inside_overlap', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['dwords_be']), ('complete', VERTS_A), 'later read wins')
    # and the decoy alone would have been a different, complete payload
    p2, _ = inputs_trace('r1_overlap_decoy', [(be_bytes(VERTS_B), 'inside', {})])
    eng2, _, _ = parse(p2)
    eq(eng2.resolves[0]['get_resolve_info_inputs']['vertex_payload']['dwords_be'], VERTS_B, 'decoy control')


@test('r1: an unrelated current read leaves the target missing 24; a same-range MemoryWrite is not a read')
def _():
    p, offs = inputs_trace('r1_unrelated', [(be_bytes(VERTS_A), 'inside_unrelated', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['missing_bytes']), ('incomplete', 24), 'unrelated read does not count')
    eq(s['unmapped_reads'], 1, 'and the target is unsupplied everywhere: one unmapped read')
    p, offs = inputs_trace('r1_write', [(be_bytes(VERTS_A), 'inside_write', {})])
    s, eng = summary(p)
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['missing_bytes'], gi['vertex_payload']['bytes_hex']),
       ('incomplete', 24, None), 'MemoryWrite never satisfies the proof')
    eq(s['memory_write_records'], 1, 'the write was still validated and counted')
    eq(s['unmapped_reads'], 1, 'a write is not input memory either')


@test('r1: reads inside an INDIRECT_BUFFER dispatch packet are discarded with it; the inner resolve is INCOMPLETE')
def _():
    tr = Trace().packet(t0(0x2208, 6)).packet(t0(0x2318, 1 << 20)).packet(vf0(VF0_ADDR))
    tr.packet_start(ib_dispatch_short(0x1F100000))
    tr.mem_read(VF0_ADDR, be_bytes(VERTS_A))          # arrives while the DISPATCH packet is pending
    tr.ib_start(0x1F100000)
    inner_off = tr.off()
    tr.packet(draw2(), base=0x1F100000)                # the resolve itself has no read of its own
    tr.ib_end().packet_end()
    s, eng = summary(tr.save('r1_ib_discard'))
    eq(s['resolves'], 1, 'one resolve')
    gi = eng.resolves[0]['get_resolve_info_inputs']
    eq((gi['status'], gi['vertex_payload']['missing_bytes'], gi['draw_packet_record_offset']),
       ('incomplete', 24, inner_off), 'dispatch-interval read discarded; provenance is the inner packet')
    eq((s['ib_dispatch_records'], s['unmapped_reads']), (1, 0), 'dispatch consumed; bytes exist globally')
    # control: the same read placed inside the inner packet is complete
    tr = Trace().packet(t0(0x2208, 6)).packet(t0(0x2318, 1 << 20)).packet(vf0(VF0_ADDR))
    tr.packet_start(ib_dispatch_short(0x1F100000)).ib_start(0x1F100000)
    tr.packet_start(draw2(), base=0x1F100000).mem_read(VF0_ADDR, be_bytes(VERTS_A)).packet_end()
    tr.ib_end().packet_end()
    s, eng = summary(tr.save('r1_ib_inner_ok'))
    eq(eng.resolves[0]['get_resolve_info_inputs']['status'], 'complete', 'inner read counts')


@test('r1: packet-local reads never leak into global behaviour: LOAD_ALU still reads the image; counters unchanged')
def _():
    vals = [fl(float(i)) for i in range(1, 9)]
    p = base_trace().packet_start(load_alu(0x1F400000, 0, 4, 8)).mem_read(0x1F400000, be_bytes(vals)).packet_end() \
        .packet(draw2()).save('r1_global')
    s, eng = summary(p)
    eq(list(eng.draws[0]['vs_consts'][4:12]), vals, 'constants loaded from the image')
    eq((s['unmapped_reads'], s['memory_read_records'], s['resolve_inputs']['total']), (0, 1, 0), 'counters')


@test('r1: a MemoryRead landing BETWEEN packets (after one PacketEnd, before the next PacketStart) is not attributed to the next draw')
def _():
    p, offs = inputs_trace('r1_between', [(be_bytes(VERTS_A), 'after', {}), (None, 'none', {})])
    s, eng = summary(p)
    a, b = (r['get_resolve_info_inputs'] for r in eng.resolves)
    eq((a['status'], a['vertex_payload']['missing_bytes']), ('incomplete', 24), 'row 1: its read came after its End')
    eq((b['status'], b['vertex_payload']['missing_bytes'], b['vertex_payload']['bytes_hex']),
       ('incomplete', 24, None), 'row 2: the between-packets read is not its own')
    eq(s['resolve_inputs'], dict(complete=0, rejected=0, incomplete=2, total=2, distinct_vertex_payloads=0,
                                 distinct_vertex_addresses=1, not_in_trace=list(X.RESOLVE_INPUTS_NOT_IN_TRACE)), 'summary')
    eq(s['unmapped_reads'], 1, 'row 1 read unsupplied bytes; row 2 found them in the image')


# ── TOOL-2B: exact guest destination ranges (schema /4) ──────────────────────
# The derivation is checked three ways: (1) against FIXED vectors produced by the
# independent C++ oracle archive/tool2b/oracle.cpp (a transcription of pinned
# draw_util.cc L926-L1206 compiled "clang++ -std=c++17 -O1 -fwrapv"; source
# SHA-256 55f67cff0c4d67838381bc679462cdfa9713776eb420e8d31be53c96cd624a37, exe
# da78f9a01e9e1d67cc25ad9f384a2c0acb9912791dfeb4035a3e9e7fa928b6db) - no Python
# was involved in producing the expected numbers; (2) through synthetic traces,
# so the /3 capture -> derivation wiring is exercised, with the oracle's view of
# each configuration retained (TRACE_ORACLE); (3) by hand-derived intermediates
# for the stages the oracle does not print (swapped bits, fixed 16.8 values,
# per-stage rectangles, granularities).

RANGE_REGS = dict(INPUT_REG_VALUES, **{
    'RB_SURFACE_INFO': (0x2000, 0x00000500), 'RB_COPY_DEST_BASE': (0x2319, 0x1F130000),
    'RB_COPY_DEST_PITCH': (0x231A, 0x02D00500), 'RB_COPY_DEST_INFO': (0x231B, 0x00000302),
    'RB_DEPTH_INFO': (0x2002, 0x000102D0), 'RB_COPY_CONTROL': (0x2318, 0x00100000),
})
RANGE_REG_ADDR = {name: addr for name, (addr, _v) in RANGE_REGS.items()}
VERTS_HALF = [fl(0.5), fl(0.5), fl(640.5), fl(0.5), fl(640.5), fl(360.5)]
VERTS_TALL = [fl(0.0), fl(0.0), fl(640.0), fl(0.0), fl(640.0), fl(16400.0)]
VERTS_ZERO = [0, 0, 0, 0, 0, 0]
VERTS_INTER = [fl(40000.0), 0x7FC00000, fl(-0.001), fl(8.001953125), fl(8.005859375), fl(-40000.0)]
VERTS_3D = [fl(8.0), fl(16.0), fl(648.0), fl(16.0), fl(648.0), fl(376.0)]
VERTS_64 = [fl(64.0), fl(64.0), fl(128.0), fl(64.0), fl(128.0), fl(96.0)]
VERTS_SMALL = [fl(0.0), fl(0.0), fl(320.0), fl(0.0), fl(320.0), fl(184.0)]
ORACLE_FIELDS = ['v0', 'v1', 'v2', 'v3', 'v4', 'v5', 'endian', 'vtx_cntl', 'sc_mode', 'wnd_off', 'wsc_tl', 'wsc_br',
                 'ssc_tl', 'ssc_br', 'surface_info', 'copy_control', 'dest_base', 'dest_pitch', 'dest_info',
                 'depth_info']
ORACLE_STATUS = {'complete': 0, 'rejected_copy_command': 1, 'rejected_msaa': 2, 'empty_rectangle': 3,
                 'non_resolvable_format': 4}
ORACLE_COMPARED = {0: 14, 4: 14, 2: 5, 3: 5, 1: 1}   # fields the oracle defines per status

# (label, the 20 inputs in ORACLE_FIELDS order, the oracle's 14 outputs:
#  status x0 y0 x1 y1 width_div_8 height_div_8 dest_format bpp_log2
#  pitch_aligned height_aligned base_adjusted extent_start extent_end)
FIXED_ORACLE_VECTORS = [
    ('endian0_pixcenter0', [0x0, 0x0, 0x44200000, 0x0, 0x44200000, 0x43B40000, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('endian1_pixcenter0', [0x0, 0x0, 0x20440000, 0x0, 0x20440000, 0xB4430000, 0x1, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('endian2_pixcenter0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('endian3_pixcenter0', [0x0, 0x0, 0x4420, 0x0, 0x4420, 0x43B4, 0x3, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('special_7FC00000_slot0_e0', [0x7FC00000, 0x41000000, 0x43A00000, 0x41000000, 0x43A00000, 0x43340000, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 8, 320, 184, 40, 22, 6, 2, 1280, 736, 521338880, 521338880, 522199040]),
    ('window_0_0_en0_dis0', [0x8041, 0x8041, 0x2044, 0x8041, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 16, 16, 640, 360, 78, 43, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('scissor_(0, 0, 1280, 720)_(0, 0, 8192, 8192)', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('pitch1280_msaa3', [0x0, 0x0, 0xA044, 0x0, 0xA044, 0x3444, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x30500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [2, 0, 0, 1280, 720, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('pitch1283_msaa0', [0x0, 0x0, 0xA044, 0x0, 0xA044, 0x3444, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x503, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 1280, 720, 160, 90, 6, 2, 1280, 736, 521338880, 521338880, 525107200]),
    ('tall', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0x409C46, 0x2, 0x0, 0x0, 0x0, 0x0, 0x3FFF3FFF, 0x0, 0x3FFF3FFF, 0x280, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 16376, 80, 2047, 6, 2, 1280, 736, 521338880, 521338880, 605143040]),
    ('empty_[0, 0, 0, 0, 0, 0]', [0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('cmd0_src0_dfmt0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x0, 0x1F130000, 0x2D00500, 0x302, 0x2D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('cmd2_src0_dfmt0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x200000, 0x1F130000, 0x2D00500, 0x302, 0x2D0], [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('fmt0_arr0', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x2, 0x102D0], [4, 8, 16, 648, 376, 80, 45, 0, 0, 1280, 736, 521338880, 521338880, 521338880]),
    ('fmt2_arr0', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x102, 0x102D0], [0, 8, 16, 648, 376, 80, 45, 2, 0, 1280, 736, 521338880, 521338880, 521812480]),
    ('fmt22_arr0', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0xB02, 0x102D0], [4, 8, 16, 648, 376, 80, 45, 22, 0, 1280, 736, 521338880, 521338880, 521338880]),
    ('3d_slice3_fmt6', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x33A, 0x102D0], [0, 8, 16, 648, 376, 80, 45, 6, 2, 1280, 736, 521338880, 521344000, 529047552]),
    ('3d_slice7_fmt2_8bpp', [0xC041, 0xC041, 0x843, 0xC041, 0x843, 0x843, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x300060, 0x17A, 0x102D0], [0, 24, 24, 136, 136, 14, 14, 2, 0, 96, 64, 521338880, 521339648, 521431168]),
    ('depth_src4_d24fs8', [0x0, 0x0, 0xA044, 0x0, 0xA044, 0x3444, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x4, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 1280, 720, 160, 90, 23, 2, 1280, 736, 521338880, 521338880, 525107200]),
    ('depth_src7_d24s8', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x7, 0x1F130000, 0x2D00500, 0x302, 0x2D0], [0, 0, 0, 640, 360, 80, 45, 22, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('fmt26_64bpp', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0xD02, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 26, 3, 1280, 736, 521338880, 521338880, 525107200]),
    ('fmt38_128bpp', [0x0, 0x0, 0xA043, 0x0, 0xA043, 0x3443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0xB40140, 0x1302, 0x102D0], [0, 0, 0, 320, 184, 40, 23, 38, 4, 320, 192, 521338880, 521338880, 522321920]),
    ('fmt24_16bpp_640sq', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0x2044, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x20280, 0x100000, 0x1F130000, 0x2800280, 0xC02, 0x102D0], [0, 0, 0, 640, 640, 80, 80, 24, 1, 640, 640, 521338880, 521338880, 522158080]),
    ('base_wrap_FFFFF000', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0xFFFFF000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 4294963200, 4294963200, 1880064]),
    ('window_neg_enabled', [0x8041, 0x8041, 0x2044, 0x8041, 0x2044, 0xB443, 0x2, 0x0, 0x10000, 0x107FF0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 32, 624, 376, 78, 43, 6, 2, 1280, 736, 521502720, 521502720, 523223040]),
    ('window_disabled_flag', [0x8041, 0x8041, 0x2044, 0x8041, 0x2044, 0xB443, 0x2, 0x0, 0x10000, 0x7ED40064, 0x80000000, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 112, 0, 744, 64, 79, 8, 6, 2, 1280, 736, 521351168, 521351168, 521601024]),
    ('scissor_clamp_600_300', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x12C0258, 0x19002BC, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 600, 296, 640, 360, 5, 8, 6, 2, 1280, 736, 522887168, 522887168, 523223040]),
    ('screen_scissor_neg', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x7FF07FF0, 0x640064, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 104, 104, 13, 13, 6, 2, 1280, 736, 521338880, 521338880, 521846784]),
    ('pixcenter1_half', [0x3F, 0x3F, 0x202044, 0x3F, 0x202044, 0x40B443, 0x2, 0x1, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('sat_pos_32768', [0x0, 0x0, 0x401C47, 0x0, 0x401C47, 0xC842, 0x2, 0x0, 0x0, 0x0, 0x0, 0x3FFF3FFF, 0x0, 0x3FFF3FFF, 0x3FF8, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 16376, 104, 2047, 13, 6, 2, 1280, 736, 521338880, 521338880, 523927552]),
    ('neg_inf_slot1', [0x41, 0x80FF, 0xA043, 0x41, 0xA043, 0x3443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 320, 184, 39, 23, 6, 2, 1280, 736, 521338880, 521338880, 522199040]),
    ('unaligned_pitch_1283x717', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2CD0503, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1312, 736, 521338880, 521338880, 523268096]),
    ('tie_even_8_plus_1_512', [0x0, 0x0, 0x80041, 0x0, 0x80041, 0x41, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 8, 8, 1, 1, 6, 2, 1280, 736, 521338880, 521338880, 521342976]),
    ('tie_odd_8_plus_3_512', [0x0, 0x0, 0x180041, 0x0, 0x180041, 0x41, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 16, 8, 2, 1, 6, 2, 1280, 736, 521338880, 521338880, 521342976]),
    ('round_up_8_plus_3_1024', [0x0, 0x0, 0xC0041, 0x0, 0xC0041, 0x41, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 16, 8, 2, 1, 6, 2, 1280, 736, 521338880, 521338880, 521342976]),
    ('reversed_vertex_order', [0x2044, 0xB443, 0x0, 0xB443, 0x0, 0x0, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('window_8_4_en0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x40008, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 640, 360, 79, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('window_8_4_en1', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x10000, 0x40008, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('window_disabled_flag_cut', [0x8041, 0x8041, 0x2044, 0x8041, 0x2044, 0xB443, 0x2, 0x0, 0x10000, 0x7ED40064, 0x80000000, 0x1680280, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 112, 0, 640, 64, 66, 8, 6, 2, 1280, 736, 521351168, 521351168, 521584640]),
    ('window_enabled_flag_shift', [0x8041, 0x8041, 0x2044, 0x8041, 0x2044, 0xB443, 0x2, 0x0, 0x10000, 0x7ED40064, 0x0, 0x1680280, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 112, 0, 744, 64, 79, 8, 6, 2, 1280, 736, 521351168, 521351168, 521601024]),
    ('wsc_padding_bits', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x40004000, 0x41404140, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 320, 320, 40, 40, 6, 2, 1280, 736, 521338880, 521338880, 522854400]),
    ('pitch1283_wide_1288', [0x0, 0x0, 0xA144, 0x0, 0xA144, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00514, 0x0, 0x20002000, 0x503, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 1280, 360, 160, 45, 6, 2, 1280, 736, 521338880, 521338880, 523304960]),
    ('3d_8bpp_x64_y64', [0x8042, 0x8042, 0x43, 0x8042, 0x43, 0xC042, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x800080, 0x11A, 0x102D0], [0, 64, 64, 128, 96, 8, 4, 2, 0, 128, 128, 521375744, 521376000, 521388032]),
    ('neg_vertex_minus8', [0xC1, 0xC1, 0xA043, 0xC1, 0xA043, 0x3443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 320, 184, 40, 23, 6, 2, 1280, 736, 521338880, 521338880, 522199040]),
    ('dest_pitch_padding_bits', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x42D04500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('ssc_br_below_tl', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x12C0258, 0x19002BC, 0x0, 0x640064, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [3, 600, 296, 600, 304, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('scissor_cuts_x1_320', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00140, 0x0, 0x20002000, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 320, 360, 40, 45, 6, 2, 1280, 736, 521338880, 521338880, 523182080]),
    ('fmt22_depth_path_src4_d24s8', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x500, 0x4, 0x1F130000, 0x2D00500, 0xC02, 0x2D0], [0, 0, 0, 640, 360, 80, 45, 22, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('pitch_clamp_x0_400', [0xC843, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x0, 0x0, 0x0, 0x0, 0x2D00500, 0x0, 0x20002000, 0x140, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [3, 320, 0, 320, 360, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
]

# the trace-driven configurations below as the oracle saw them
TRACE_ORACLE = {label: (inputs, outputs) for label, inputs, outputs in [
    ('base_A', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('base_B', [0x8041, 0x41, 0xA043, 0x41, 0xA043, 0x3443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 24, 8, 328, 184, 38, 22, 6, 2, 1280, 736, 521338880, 521338880, 522203136]),
    ('endian0_A', [0x0, 0x0, 0x44200000, 0x0, 0x44200000, 0x43B40000, 0x0, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('endian1_A', [0x0, 0x0, 0x20440000, 0x0, 0x20440000, 0xB4430000, 0x1, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('endian3_A', [0x0, 0x0, 0x4420, 0x0, 0x4420, 0x43B4, 0x3, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('pixcenter1_half', [0x3F, 0x3F, 0x202044, 0x3F, 0x202044, 0x40B443, 0x2, 0x101, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('pixcenter0_half', [0x3F, 0x3F, 0x202044, 0x3F, 0x202044, 0x40B443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 656, 368, 81, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('window_disabled', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x1, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 0, 640, 360, 80, 45, 6, 2, 1280, 736, 521338880, 521338880, 523223040]),
    ('window_neg', [0x8041, 0x41, 0xA043, 0x41, 0xA043, 0x3443, 0x2, 0x100, 0x10001, 0x107FF0, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 0, 24, 304, 200, 38, 22, 6, 2, 1280, 736, 521338880, 521338880, 522362880]),
    ('scissor_window_no_disable', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x0, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('scissor_cut_600_300', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x812C0258, 0x19002BC, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 600, 296, 648, 368, 6, 9, 6, 2, 1280, 736, 522887168, 522887168, 523227136]),
    ('screen_scissor_neg', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x7FF07FF0, 0x640064, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 104, 104, 12, 13, 6, 2, 1280, 736, 521338880, 521338880, 521846784]),
    ('msaa3', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x30500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [2, 8, 0, 648, 368, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('msaa2', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x20500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('pitch320', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x140, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 320, 368, 39, 46, 6, 2, 1280, 736, 521338880, 521338880, 523182080]),
    ('pitch1283', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x503, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('tall', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0x208046, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x3FFF3FFF, 0x10002, 0x3FFF3FFF, 0x3FF8, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 16376, 80, 2047, 6, 2, 1280, 736, 521338880, 521338880, 605147136]),
    ('empty', [0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [3, 8, 0, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('cmd2', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x200000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('cmd3', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x300000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('cmd0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x0, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('depth_src4_d24fs8', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x4, 0x1F130000, 0x2D00500, 0xC02, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 23, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('depth_src7_d24s8', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x7, 0x1F130000, 0x2D00500, 0xC02, 0x2D0], [0, 8, 0, 648, 368, 80, 46, 22, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('fmt0', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x2, 0x102D0], [4, 8, 0, 648, 368, 80, 46, 0, 0, 1280, 736, 521338880, 521338880, 521338880]),
    ('fmt22_color', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0xB02, 0x102D0], [4, 8, 0, 648, 368, 80, 46, 22, 0, 1280, 736, 521338880, 521338880, 521338880]),
    ('fmt2_8bpp', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x102, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 2, 0, 1280, 736, 521338880, 521338880, 521812480]),
    ('fmt24_16bpp', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0xC02, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 24, 1, 1280, 736, 521338880, 521338880, 522284032]),
    ('fmt26_64bpp', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0xD02, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 26, 3, 1280, 736, 521338880, 521338880, 525115392]),
    ('fmt38_128bpp', [0x0, 0x0, 0xA043, 0x0, 0xA043, 0x3843, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0xB40140, 0x1302, 0x102D0], [0, 8, 0, 328, 192, 40, 24, 38, 4, 320, 192, 521338880, 521338880, 522338304]),
    ('3d_slice3', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x33A, 0x102D0], [0, 16, 16, 656, 384, 80, 46, 6, 2, 1280, 736, 521338880, 521344000, 529047552]),
    ('3d_slice0', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x30A, 0x102D0], [0, 16, 16, 656, 384, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 529047552]),
    ('2d_same_rect', [0x41, 0x8041, 0x2244, 0x8041, 0x2244, 0xBC43, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 16, 16, 656, 384, 80, 46, 6, 2, 1280, 736, 521338880, 521338880, 523227136]),
    ('3d_8bpp_64', [0x8042, 0x8042, 0x43, 0x8042, 0x43, 0xC042, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x800080, 0x11A, 0x102D0], [0, 72, 64, 136, 104, 8, 5, 2, 0, 128, 128, 521375744, 521376000, 521407488]),
    ('base_wrap', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0xFFFFF000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1280, 736, 4294963200, 4294963200, 1884160]),
    ('unaligned_1283x717', [0x0, 0x0, 0x2044, 0x0, 0x2044, 0xB443, 0x2, 0x100, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2CD0503, 0x302, 0x102D0], [0, 8, 0, 648, 368, 80, 46, 6, 2, 1312, 736, 521338880, 521338880, 523272192]),
    ('intermediates', [0x401C47, 0xC07F, 0x6F1283BA, 0x80041, 0x180041, 0x401CC7, 0x2, 0x101, 0x10001, 0x40008, 0x80000000, 0x2D00500, 0x10002, 0x1FFF1FFF, 0x500, 0x100000, 0x1F130000, 0x2D00500, 0x302, 0x102D0], [0, 8, 0, 1280, 16, 159, 2, 6, 2, 1280, 736, 521338880, 521338880, 521502720]),
]}

# label -> (guest float bits, vertex fetch endian, raw register overrides)
TRACE_CASES = {
    'base_A': (VERTS_A, 2, {}),
    'base_B': (VERTS_B, 2, {}),
    'endian0_A': (VERTS_A, 0, {}),
    'endian1_A': (VERTS_A, 1, {}),
    'endian3_A': (VERTS_A, 3, {}),
    'pixcenter1_half': (VERTS_HALF, 2, dict(PA_SU_VTX_CNTL=0x00000101)),
    'pixcenter0_half': (VERTS_HALF, 2, {}),
    'window_disabled': (VERTS_A, 2, dict(PA_SU_SC_MODE_CNTL=0x00000001)),
    'window_neg': (VERTS_B, 2, dict(PA_SC_WINDOW_OFFSET=0x00107FF0)),
    'scissor_window_no_disable': (VERTS_A, 2, dict(PA_SC_WINDOW_SCISSOR_TL=0x00000000)),
    'scissor_cut_600_300': (VERTS_A, 2, dict(PA_SC_WINDOW_SCISSOR_TL=0x812C0258, PA_SC_WINDOW_SCISSOR_BR=0x019002BC)),
    'screen_scissor_neg': (VERTS_A, 2, dict(PA_SC_SCREEN_SCISSOR_TL=0x7FF07FF0, PA_SC_SCREEN_SCISSOR_BR=0x00640064)),
    'msaa3': (VERTS_A, 2, dict(RB_SURFACE_INFO=0x00030500)),
    'msaa2': (VERTS_A, 2, dict(RB_SURFACE_INFO=0x00020500)),
    'pitch320': (VERTS_A, 2, dict(RB_SURFACE_INFO=0x00000140)),
    'pitch1283': (VERTS_A, 2, dict(RB_SURFACE_INFO=0x00000503)),
    'tall': (VERTS_TALL, 2, dict(PA_SC_WINDOW_SCISSOR_BR=0x3FFF3FFF, PA_SC_SCREEN_SCISSOR_BR=0x3FFF3FFF,
                                 RB_SURFACE_INFO=0x00003FF8)),
    'empty': (VERTS_ZERO, 2, {}),
    'cmd2': (VERTS_A, 2, dict(RB_COPY_CONTROL=0x00200000)),
    'cmd3': (VERTS_A, 2, dict(RB_COPY_CONTROL=0x00300000)),
    'cmd0': (VERTS_A, 2, dict(RB_COPY_CONTROL=0x00000000)),
    'depth_src4_d24fs8': (VERTS_A, 2, dict(RB_COPY_CONTROL=0x00000004, RB_DEPTH_INFO=0x000102D0, RB_COPY_DEST_INFO=0xC02)),
    'depth_src7_d24s8': (VERTS_A, 2, dict(RB_COPY_CONTROL=0x00000007, RB_DEPTH_INFO=0x000002D0, RB_COPY_DEST_INFO=0xC02)),
    'fmt0': (VERTS_A, 2, dict(RB_COPY_DEST_INFO=0x002)),
    'fmt22_color': (VERTS_A, 2, dict(RB_COPY_DEST_INFO=0xB02)),
    'fmt2_8bpp': (VERTS_A, 2, dict(RB_COPY_DEST_INFO=0x102)),
    'fmt24_16bpp': (VERTS_A, 2, dict(RB_COPY_DEST_INFO=0xC02)),
    'fmt26_64bpp': (VERTS_A, 2, dict(RB_COPY_DEST_INFO=0xD02)),
    'fmt38_128bpp': (VERTS_SMALL, 2, dict(RB_COPY_DEST_INFO=0x1302, RB_COPY_DEST_PITCH=0x00B40140)),
    '3d_slice3': (VERTS_3D, 2, dict(RB_COPY_DEST_INFO=0x33A)),
    '3d_slice0': (VERTS_3D, 2, dict(RB_COPY_DEST_INFO=0x30A)),
    '2d_same_rect': (VERTS_3D, 2, dict(RB_COPY_DEST_INFO=0x302)),
    '3d_8bpp_64': (VERTS_64, 2, dict(RB_COPY_DEST_INFO=0x11A, RB_COPY_DEST_PITCH=0x00800080)),
    'base_wrap': (VERTS_A, 2, dict(RB_COPY_DEST_BASE=0xFFFFF000)),
    'unaligned_1283x717': (VERTS_A, 2, dict(RB_COPY_DEST_PITCH=0x02CD0503)),
    'intermediates': (VERTS_INTER, 2, dict(PA_SU_VTX_CNTL=0x00000101)),
}
COMPLETE_ROW_KEYS = ['derivation_version', 'status', 'reason', 'join_usable', 'copy_command', 'vertices_swapped_bits',
                     'half_pixel_offset', 'vertices_fixed_16_8', 'rect_pixels', 'window_offset',
                     'rect_after_window_offset', 'scissor', 'rect_after_scissor', 'rect_aligned_8', 'msaa_samples',
                     'surface_pitch_aligned', 'rect_after_pitch_clamp', 'rect_final', 'width_div_8', 'height_div_8',
                     'destination', 'tiling', 'range', 'raw_vs_derived']
NO_ARITHMETIC_KEYS = ['derivation_version', 'status', 'reason', 'join_usable', 'copy_command']


def rr(**over):
    """RANGE_REGS with named raw overrides."""
    regs = dict(RANGE_REGS)
    for name, val in over.items():
        regs[name] = (RANGE_REG_ADDR[name], val)
    return regs


def swapped_payload(verts, endian):
    """The 24 payload bytes guest memory must hold for a fetch of `endian` to
    yield the float bits `verts` (independent transcription of xenos.h
    L1092-L1109; every mode is an involution; the host reads little-endian)."""
    out = []
    for v in verts:
        if endian == 1:
            m = ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF)
        elif endian == 2:
            m = int.from_bytes(v.to_bytes(4, 'big'), 'little')
        elif endian == 3:
            m = ((v >> 16) & 0xFFFF) | ((v << 16) & 0xFFFF0000)
        else:
            m = v
        out.append(m)
    return struct.pack('<6I', *out)


def range_trace(name, rows):
    """A kCopy-mode trace whose resolves each carry their own register state:
    rows = [(payload_bytes_or_None, placement 'inside'|'none'|'after', vf0_kwargs,
    register overrides)]. Every RANGE_REGS register is rewritten before each
    draw. Returns (path, [PacketStart record offsets])."""
    tr = Trace().packet(t0(0x2208, 6)).packet(t0(0x2318, 1 << 20))
    for _name, (reg, val) in RANGE_REGS.items():
        tr.packet(t0(reg, val))
    offsets = []
    for payload, place, kw, over in rows:
        for _name, (reg, val) in rr(**over).items():
            tr.packet(t0(reg, val))
        addr = kw.get('addr', VF0_ADDR)
        tr.packet(vf0(addr, kw.get('size', 6), kw.get('endian', 2), kw.get('type', 3)))
        offsets.append(tr.off())
        tr.packet_start(draw2())
        if payload is not None and place == 'inside':
            tr.mem_read(addr, payload)
        tr.packet_end()
        if payload is not None and place == 'after':
            tr.mem_read(addr, payload)
    return tr.save(name), offsets


def range_rows(specs):
    rows = []
    for label, place, vf in specs:
        verts, endian, over = TRACE_CASES[label]
        kw = dict(endian=endian)
        kw.update(vf or {})
        rows.append((swapped_payload(verts, endian), place, kw, over))
    return rows


def range_case(label, place='inside', vf=None):
    """One-row trace for a TRACE_CASES label -> (path, summary, engine, row, derived)."""
    p, _offs = range_trace('t2b_' + label + ('' if place == 'inside' and not vf else '_' + place + str(sorted((vf or {}).items()))),
                           range_rows([(label, place, vf)]))
    s, eng = summary(p)
    return p, s, eng, eng.resolves[0], eng.resolves[0]['guest_destination_range']


def derive_vec(inputs):
    payload = struct.pack('<6I', *inputs[:6])
    return X.derive_guest_destination_range(inputs[15], payload, inputs[6], inputs[7], inputs[8], inputs[9],
                                            inputs[10], inputs[11], inputs[12], inputs[13], inputs[14],
                                            inputs[16], inputs[17], inputs[18], inputs[19])


def oracle_projection(g):
    """A derived object projected onto the oracle's 14 output fields."""
    code = ORACLE_STATUS[g['status']]
    if code == 1:
        return [1]
    out = [code] + list(g['rect_final'] if 'rect_final' in g else g['rect_aligned_8'])
    if code in (2, 3):
        return out
    dst, rg = g['destination'], g['range']
    return out + [g['width_div_8'], g['height_div_8'], dst['format'],
                  g['tiling']['bytes_per_block_log2'] if g['tiling'] else 0,
                  dst['pitch_aligned'], dst['height_aligned'],
                  rg['adjusted_base'], rg['extent_start'], rg['extent_end_exclusive']]


def oracle_check(label, g, expected):
    n = ORACLE_COMPARED[expected[0]]
    got = oracle_projection(g)
    eq(got[:n], expected[:n], '%s: port vs C++ oracle (%d fields)' % (label, n))


def oracle_inputs_of_row(r):
    """The 20 oracle inputs read back from a schema-/3 row and its /2 copy
    registers: proof that the derivation consumes nothing outside the row."""
    gi = r['get_resolve_info_inputs']
    regs = gi['registers']
    return list(struct.unpack('<6I', bytes.fromhex(gi['vertex_payload']['bytes_hex']))) + [
        gi['vertex_fetch_0']['endian'], regs['PA_SU_VTX_CNTL'], regs['PA_SU_SC_MODE_CNTL'],
        regs['PA_SC_WINDOW_OFFSET'], regs['PA_SC_WINDOW_SCISSOR_TL'], regs['PA_SC_WINDOW_SCISSOR_BR'],
        regs['PA_SC_SCREEN_SCISSOR_TL'], regs['PA_SC_SCREEN_SCISSOR_BR'], r['surface_info'], r['copy_control'],
        r['dest_base'], r['dest_pitch'], r['dest_info'], r['depth_info']]


@test('T2B2 fixed C++ oracle vectors: the Python port reproduces the independent oracle field for field on all 49 retained vectors (four endian modes, float edges and ties, signed window/scissor, alignment, clamps, empty, commands, MSAA, 2D/3D, bpp classes, uint32 wrap)')
def _():
    labels = [l for l, _i, _o in FIXED_ORACLE_VECTORS]
    eq(len(labels), 49, 'vector count')
    eq(len(set(labels)), 49, 'labels unique')
    eq(sorted({o[0] for _l, _i, o in FIXED_ORACLE_VECTORS}), [0, 1, 2, 3, 4], 'every oracle status class present')
    eq(sorted({i[6] for _l, i, _o in FIXED_ORACLE_VECTORS}), [0, 1, 2, 3], 'all four endian modes present')
    for label, inputs, expected in FIXED_ORACLE_VECTORS:
        eq(len(inputs), 20, '%s: 20 inputs' % label)
        oracle_check(label, derive_vec(inputs), expected)


@test('T2B1 wiring: for all 36 trace cases the 20 oracle inputs read back from the /3 row (payload bytes, fetch endian, the seven registers) plus the /2 copy registers equal the oracle input vector, and the derived row equals the oracle output')
def _():
    eq(sorted(TRACE_CASES), sorted(TRACE_ORACLE), 'every case has an oracle record')
    eq(len(TRACE_CASES), 36, 'case count')
    for label in sorted(TRACE_CASES):
        _p, s, _e, r, g = range_case(label)
        inputs, outputs = TRACE_ORACLE[label]
        eq(oracle_inputs_of_row(r), inputs, '%s: the /3 row holds exactly the oracle inputs' % label)
        oracle_check(label, g, outputs)
        eq(s['guest_destination_ranges']['total'], 1, '%s: census total' % label)


@test('T2B complete row: every intermediate of the base case is the hand-derived pinned value (half pixel 0.5, 16.8 fixed, (v+127)>>8, window offset (8,4), scissor (2,1)-(1280,720) under window_offset_disable, 8-alignment, 32-aligned destination, 2D tiling, 1888256-byte extent) and the row shape is exact')
def _():
    _p, _s, _e, _r, g = range_case('base_A')
    eq(list(g), COMPLETE_ROW_KEYS, 'row keys, in order')
    eq((g['derivation_version'], g['status'], g['reason'], g['join_usable'], g['copy_command']),
       ('xtr_draws-resolve-range/1', 'complete', None, True, 1), 'head')
    eq(g['vertices_swapped_bits'], ['%08X' % v for v in VERTS_A], 'swapped bits = the guest floats')
    eq(g['half_pixel_offset'], 0.5, 'PA_SU_VTX_CNTL pix_center 0 = kD3DZero -> +0.5')
    eq(g['vertices_fixed_16_8'], [128, 128, 163968, 128, 163968, 92288], '16.8 fixed of v+0.5')
    eq(g['rect_pixels'], [0, 0, 640, 360], '(v+127)>>8')
    eq(g['window_offset'], dict(enable=1, x=8, y=4), 'window offset decoded signed')
    eq(g['rect_after_window_offset'], [8, 4, 648, 364], 'offset applied (vtx_window_offset_enable)')
    eq(g['scissor'], dict(offset=[2, 1], extent=[1278, 719]),
       'GetScissor<false>: window scissor (0,0)-(1280,720) with window_offset_disable, screen scissor tl (2,1)')
    eq(g['rect_after_scissor'], [8, 4, 648, 364], 'inside the scissor')
    eq(g['rect_aligned_8'], [8, 0, 648, 368], 'x0&=~7, y0&=~7, align(x1,8), align(y1,8)')
    eq((g['msaa_samples'], g['surface_pitch_aligned']), (0, 1280), 'MSAA 1x, surface pitch 1280')
    eq((g['rect_after_pitch_clamp'], g['rect_final']), ([8, 0, 648, 368], [8, 0, 648, 368]), 'no pitch/height clamp')
    eq((g['width_div_8'], g['height_div_8']), (80, 46), 'div 8')
    eq(g['destination'], dict(is_depth=False, format=6, format_name='k_8_8_8_8', format_type='kResolvable',
                              bits_per_pixel=32, pitch_aligned=1280, height_aligned=736, pitch_aligned_div_32=40,
                              height_aligned_div_32=23), 'destination')
    eq(g['tiling'], dict(mode='2D', bytes_per_block_log2=2, slice=0, x_base_granularity_log2=5,
                         y_base_granularity_log2=5, dest_base_x=0, dest_base_y=0, offset_x_div_8=1,
                         offset_y_div_8=0), 'tiling')
    eq(g['range'], dict(raw_base=0x1F130000, adjusted_base=0x1F130000, extent_start=0x1F130000,
                        extent_length=1888256, extent_end_exclusive=0x1F2FD000, invariants_hold=True), 'range')
    eq(g['raw_vs_derived'], dict(adjusted_base_equals_raw=True, extent_start_equals_raw_base=True,
                                 adjusted_minus_raw=0, extent_start_minus_raw=0,
                                 declared_linear_span_bytes=3686400, extent_equals_declared_linear_span=False),
       'raw vs derived')
    js = json.dumps(g)
    for banned in ('resolution_scale', 'exponent', 'fill_half_pixel', 'truncated', 'github.com', 'cvar'):
        ck(banned not in js, 'no host-dependent or citation field in the row: %s' % banned)


@test('T2B endian: the same guest floats stored for fetch endian kNone/k8in16/k8in32/k16in32 derive identical swapped bits and identical ranges; the /3 fetch endian selects the swap')
def _():
    rows = [range_case(l)[4] for l in ('endian0_A', 'endian1_A', 'base_A', 'endian3_A')]
    for g in rows:
        eq(g['vertices_swapped_bits'], ['%08X' % v for v in VERTS_A], 'swapped bits identical')
        eq(g['rect_final'], [8, 0, 648, 368], 'rect identical')
    eq(len({json.dumps(g['range']) for g in rows}), 1, 'one range')


@test('T2B pixel center: pix_center=1 (kOGLHalf) adds nothing, so half-pixel vertices land where integer vertices land under pix_center=0; pix_center=0 on the same half-pixel vertices moves x1 to 656 (draw_util.cc L963-L968)')
def _():
    _p, _s, _e, _r, g1 = range_case('pixcenter1_half')
    _p, _s, _e, _r, g0 = range_case('pixcenter0_half')
    eq((g1['half_pixel_offset'], g1['vertices_fixed_16_8'], g1['rect_pixels'], g1['rect_final']),
       (0.0, [128, 128, 163968, 128, 163968, 92288], [0, 0, 640, 360], [8, 0, 648, 368]), 'kOGLHalf')
    eq((g0['half_pixel_offset'], g0['vertices_fixed_16_8'], g0['rect_pixels'], g0['rect_final']),
       (0.5, [256, 256, 164096, 256, 164096, 92416], [1, 1, 641, 361], [8, 0, 656, 368]), 'kD3DZero')


@test('T2B2 FloatToD3D11Fixed16p8 through the row: saturation both signs, NaN -> 0, |x| < 1/512 -> 0, ties to even at 2048.5 and 2049.5 (graphics_util.cc L17-L60); the rectangle then clips to the scissor')
def _():
    _p, _s, _e, _r, g = range_case('intermediates')
    eq(g['vertices_fixed_16_8'], [8388607, 0, 0, 2048, 2050, -8388608], 'fixed')
    eq(g['rect_pixels'], [0, -32768, 32768, 8], 'min/max then (v+127)>>8')
    eq(g['rect_final'], [8, 0, 1280, 16], 'clipped')


@test('T2B window offset: vtx_window_offset_enable=0 leaves the rectangle unshifted; a negative 15-bit offset (-16,16) is sign-extended (registers.h L610-L620) and the screen scissor tl (2,1) then clips x0')
def _():
    _p, _s, _e, _r, g = range_case('window_disabled')
    eq((g['window_offset'], g['rect_after_window_offset'], g['rect_final']),
       (dict(enable=0, x=8, y=4), [0, 0, 640, 360], [0, 0, 640, 360]), 'disabled')
    _p, _s, _e, _r, g = range_case('window_neg')
    eq((g['window_offset'], g['rect_pixels'], g['rect_after_window_offset'], g['rect_after_scissor'], g['rect_final']),
       (dict(enable=1, x=-16, y=16), [16, 8, 320, 180], [0, 24, 304, 196], [2, 24, 304, 196], [0, 24, 304, 200]),
       'negative')


@test('T2B scissor: window_offset_disable clear shifts the window scissor by the window offset; a (600,300)-(700,400) window scissor cuts to (600,296)-(648,368) and moves the adjusted base; a negative screen scissor tl clamps to 0 and its br (100,100) cuts (draw_util.cc L636-L724)')
def _():
    _p, _s, _e, _r, g = range_case('scissor_window_no_disable')
    eq((g['scissor'], g['rect_final']), (dict(offset=[8, 4], extent=[1280, 720]), [8, 0, 648, 368]), 'shifted scissor')
    _p, _s, _e, _r, g = range_case('scissor_cut_600_300')
    eq((g['scissor'], g['rect_after_scissor'], g['rect_final']),
       (dict(offset=[600, 300], extent=[100, 100]), [600, 300, 648, 364], [600, 296, 648, 368]), 'cut')
    eq((g['tiling']['dest_base_x'], g['tiling']['dest_base_y'], g['range']['adjusted_base'],
        g['raw_vs_derived']['adjusted_base_equals_raw'], g['raw_vs_derived']['adjusted_minus_raw'],
        g['range']['extent_start'], g['raw_vs_derived']['extent_start_equals_raw_base']),
       (576, 288, 522887168, False, 1548288, 522887168, False),
       'T2B3: the adjusted base and the extent start move with the 32-aligned origin')
    _p, _s, _e, _r, g = range_case('screen_scissor_neg')
    eq((g['scissor'], g['rect_final']), (dict(offset=[0, 0], extent=[100, 100]), [8, 0, 104, 104]), 'negative screen scissor')


@test('T2B MSAA: 8x (RB_SURFACE_INFO msaa_samples=3) is rejected by name with the 8-aligned rectangle kept and no range; 4x passes (L1029-L1038)')
def _():
    _p, s, _e, _r, g = range_case('msaa3')
    eq((g['status'], g['join_usable'], g['msaa_samples'], g['rect_aligned_8'], 'range' in g, 'surface_pitch_aligned' in g),
       ('rejected_msaa', False, 3, [8, 0, 648, 368], False, False), 'rejected')
    ck('8x MSAA' in g['reason'] and 'L1029-L1038' in g['reason'], 'reason names Xenia: %r' % g['reason'])
    eq(s['guest_destination_ranges']['rejected_msaa'], 1, 'census')
    _p, _s, _e, _r, g = range_case('msaa2')
    eq((g['status'], g['msaa_samples'], g['rect_final']), ('complete', 2, [8, 0, 648, 368]), '4x passes')


@test('T2B surface pitch clamp: pitch 320 clamps x1 (and x0 when beyond) to 320; pitch 1283 is 8-aligned down to 1280 and does not clamp 648 (L1040-L1049)')
def _():
    _p, _s, _e, _r, g = range_case('pitch320')
    eq((g['surface_pitch_aligned'], g['rect_aligned_8'], g['rect_after_pitch_clamp'], g['rect_final'], g['width_div_8']),
       (320, [8, 0, 648, 368], [8, 0, 320, 368], [8, 0, 320, 368], 39), 'clamped')
    _p, _s, _e, _r, g = range_case('pitch1283')
    eq((g['surface_pitch_aligned'], g['rect_final']), (1280, [8, 0, 648, 368]), 'not clamped')


@test('T2B height clamp: a rectangle the scissor leaves 16384 rows tall is cut to kMaxResolveSize 16376 (L1053-L1059; xenos.h L1027-L1029)')
def _():
    _p, _s, _e, _r, g = range_case('tall')
    eq((g['rect_after_pitch_clamp'], g['rect_final'], g['height_div_8'], X.MAX_RESOLVE_SIZE),
       ([8, 0, 648, 16384], [8, 0, 648, 16376], 2047, 16376), 'tall')


@test('T2B empty rectangle: zero vertices become (8,0)-(8,8) after offset and alignment; x0 >= x1 -> empty_rectangle with width_div_8 = height_div_8 = 0, no range, not join-usable, counted separately (L1065-L1069)')
def _():
    _p, s, _e, _r, g = range_case('empty')
    eq((g['status'], g['join_usable'], g['rect_final'], g['width_div_8'], g['height_div_8'], 'range' in g, 'destination' in g),
       ('empty_rectangle', False, [8, 0, 8, 8], 0, 0, False, False), 'empty')
    ck('L1065-L1069' in g['reason'], 'reason cites the early return')
    gr = s['guest_destination_ranges']
    eq((gr['empty_rectangle'], gr['complete'], gr['total'], gr['join_usable']), (1, 0, 1, 0), 'census')


@test('T2B copy command: kNull(2)/kConstantOne(3) are rejected by name BEFORE any arithmetic (no vertex, rectangle or range keys) with a complete payload and with no payload at all; kRaw(0) derives the same range as kConvert(1) (L939-L947)')
def _():
    for label, cmd in (('cmd2', 2), ('cmd3', 3)):
        _p, s, _e, r, g = range_case(label)
        eq(list(g), NO_ARITHMETIC_KEYS, '%s: keys' % label)
        eq((g['status'], g['join_usable'], g['copy_command'], r['get_resolve_info_inputs']['status']),
           ('rejected_copy_command', False, cmd, 'complete'), '%s: status' % label)
        ck(('Unsupported resolve copy command %d' % cmd) in g['reason'] and 'L939-L947' in g['reason'],
           '%s: reason %r' % (label, g['reason']))
        eq(s['guest_destination_ranges']['rejected_copy_command'], 1, '%s: census' % label)
    _p, _s, _e, r, g = range_case('cmd2', place='none')
    eq((g['status'], r['get_resolve_info_inputs']['status']), ('rejected_copy_command', 'incomplete'),
       'the command check precedes the fetch/payload check')
    _p, _s, _e, _r, g0 = range_case('cmd0')
    _p, _s, _e, _r, g1 = range_case('base_A')
    eq((g0['copy_command'], g0['status'], g0['range']), (0, 'complete', g1['range']), 'kRaw derives the same range')


@test('T2B fail closed: a /3 rejected fetch propagates as inputs_rejected and a /3 incomplete payload as inputs_incomplete, each with no arithmetic keys and a reason naming the /3 status; census conserved')
def _():
    _p, s, _e, r, g = range_case('base_A', vf=dict(type=2))
    eq((r['get_resolve_info_inputs']['status'], g['status'], g['join_usable'], list(g)),
       ('rejected', 'inputs_rejected', False, NO_ARITHMETIC_KEYS), 'rejected')
    ck(g['reason'].startswith('no arithmetic') and '/3 inputs are rejected' in g['reason'] and 'bad type' in g['reason'],
       'reason %r' % g['reason'])
    eq((s['guest_destination_ranges']['inputs_rejected'], s['guest_destination_ranges']['total']), (1, 1), 'census rejected')
    _p, s, _e, r, g = range_case('base_A', place='none')
    eq((r['get_resolve_info_inputs']['status'], g['status'], g['join_usable'], list(g)),
       ('incomplete', 'inputs_incomplete', False, NO_ARITHMETIC_KEYS), 'incomplete')
    ck(g['reason'].startswith('no arithmetic') and '/3 inputs are incomplete' in g['reason']
       and 'not fully supplied' in g['reason'], 'reason %r' % g['reason'])
    eq((s['guest_destination_ranges']['inputs_incomplete'], s['guest_destination_ranges']['total'], s['resolves']),
       (1, 1, 1), 'census incomplete')


@test('T2B depth: copy_src_select 4..7 resolves the depth buffer and the destination format comes from RB_DEPTH_INFO (kD24FS8 -> k_24_8_FLOAT 23, kD24S8 -> k_24_8 22), the raw RB_COPY_DEST_INFO format (24) being ignored; both complete 32-bpp 2D ranges (L1082-L1108; texture_info.h L170-L181)')
def _():
    for label, fmt, name in (('depth_src4_d24fs8', 23, 'k_24_8_FLOAT'), ('depth_src7_d24s8', 22, 'k_24_8')):
        _p, _s, _e, r, g = range_case(label)
        d = g['destination']
        eq((r['dest']['fmt'], r['source']['kind'], g['status'], d['is_depth'], d['format'], d['format_name'],
            d['format_type'], d['bits_per_pixel'], g['tiling']['bytes_per_block_log2'], g['join_usable']),
           (24, 'depth', 'complete', True, fmt, name, 'kUncompressed', 32, 2, True), label)
        eq(g['range']['extent_length'], 1888256, '%s: same 32-bpp extent as the colour base case' % label)


@test('T2B non-resolvable destination format: k_1_REVERSE(0) and the colour-path k_24_8(22) are kUncompressed -> non_resolvable_format, zero extent at the raw base, tiling null, not join-usable, named; the same 22 on the depth path is complete (is_depth || kResolvable; L1146-L1200)')
def _():
    for label, fmt, name in (('fmt0', 0, 'k_1_REVERSE'), ('fmt22_color', 22, 'k_24_8')):
        _p, s, _e, _r, g = range_case(label)
        eq((g['status'], g['join_usable'], g['tiling'], g['destination']['format'], g['destination']['format_name'],
            g['destination']['format_type']), ('non_resolvable_format', False, None, fmt, name, 'kUncompressed'), label)
        eq(g['range'], dict(raw_base=0x1F130000, adjusted_base=0x1F130000, extent_start=0x1F130000, extent_length=0,
                            extent_end_exclusive=0x1F130000, invariants_hold=True), '%s: zero extent' % label)
        ck(('%s(%d)' % (name, fmt)) in g['reason'] and 'ZERO extent' in g['reason'] and 'L1195-L1200' in g['reason'],
           '%s: reason %r' % (label, g['reason']))
        gr = s['guest_destination_ranges']
        eq((gr['non_resolvable_format'], gr['join_usable'], gr['extent_differs_from_declared_linear_span']), (1, 0, 1),
           '%s: census' % label)
    _p, _s, _e, _r, g = range_case('depth_src7_d24s8')
    eq((g['destination']['format'], g['status'], g['join_usable']), (22, 'complete', True), 'depth path 22 is resolvable')


@test('T2B bytes-per-block classes: 8/16/32/64/128 bpp give bpp_log2 0..4 and base granularities (7,7)/(6,6)/(5,5)/(5,5)/(5,5) (xenos.h L1211-L1218); the extents follow the pinned upper-bound table (texture_util.cc L450-L477)')
def _():
    exp = {'fmt2_8bpp': (2, 'k_8', 8, 0, 7, 7, 473600), 'fmt24_16bpp': (24, 'k_16', 16, 1, 6, 6, 945152),
           'base_A': (6, 'k_8_8_8_8', 32, 2, 5, 5, 1888256), 'fmt26_64bpp': (26, 'k_16_16_16_16', 64, 3, 5, 5, 3776512),
           'fmt38_128bpp': (38, 'k_32_32_32_32_FLOAT', 128, 4, 5, 5, 999424)}
    for label, (fmt, name, bpp, l2, xg, yg, length) in exp.items():
        _p, _s, _e, _r, g = range_case(label)
        d, t = g['destination'], g['tiling']
        eq((d['format'], d['format_name'], d['bits_per_pixel'], t['bytes_per_block_log2'], t['x_base_granularity_log2'],
            t['y_base_granularity_log2'], g['range']['extent_length'], g['status'], g['join_usable']),
           (fmt, name, bpp, l2, xg, yg, length, 'complete', True), label)


@test('T2B 3D destination: copy_dest_array selects Tiled3D with y granularity 5; the slice moves the lower bound, not the adjusted base; the same rectangle in 2D gives a different extent; an 8-bpp 3D origin at (64,64) has x granularity 6 and an adjusted base 0x9000 past the raw base (texture_address.h L213-L238; texture_util.h L302-L312; texture_util.cc L479-L510)')
def _():
    _p, _s, _e, _r, g3 = range_case('3d_slice3')
    _p, _s, _e, _r, g0 = range_case('3d_slice0')
    _p, _s, _e, _r, g2 = range_case('2d_same_rect')
    eq((g3['tiling']['mode'], g3['tiling']['slice'], g3['tiling']['x_base_granularity_log2'],
        g3['tiling']['y_base_granularity_log2']), ('3D', 3, 5, 5), '3D slice 3')
    eq((g0['tiling']['mode'], g0['tiling']['slice'], g2['tiling']['mode'], g2['tiling']['slice']), ('3D', 0, '2D', 0), 'modes')
    eq((g3['rect_final'], g0['rect_final'], g2['rect_final']), ([16, 16, 656, 384],) * 3, 'same rectangle')
    eq((g3['range']['adjusted_base'], g0['range']['adjusted_base'], g2['range']['adjusted_base']), (0x1F130000,) * 3,
       'adjusted base independent of the slice')
    eq((g3['range']['extent_start'] - 0x1F130000, g0['range']['extent_start'] - 0x1F130000), (5120, 0), 'slice 3 lower bound')
    eq((g3['range']['extent_end_exclusive'], g0['range']['extent_end_exclusive'], g2['range']['extent_end_exclusive']),
       (529047552, 529047552, 523227136), '3D upper bound; 2D differs')
    _p, _s, _e, _r, g = range_case('3d_8bpp_64')
    t = g['tiling']
    eq((t['mode'], t['bytes_per_block_log2'], t['x_base_granularity_log2'], t['y_base_granularity_log2'], t['dest_base_x'],
        t['dest_base_y'], t['offset_x_div_8'], t['offset_y_div_8'], t['slice']), ('3D', 0, 6, 5, 64, 64, 1, 0, 1),
       '8-bpp 3D granularity')
    eq((g['rect_final'], g['range']['raw_base'], g['range']['adjusted_base'], g['raw_vs_derived']['adjusted_minus_raw'],
        g['range']['extent_start'], g['raw_vs_derived']['adjusted_base_equals_raw']),
       ([72, 64, 136, 104], 0x1F130000, 521375744, 36864, 521376000, False), 'T2B3: adjusted base differs from raw')


@test('T2B uint32 wrap: dest base 0xFFFFF000 keeps 32-bit arithmetic (end wraps to 0x001CC000 < start): invariants_hold=false and join_usable=false while status stays complete (L1201-L1206 are debug asserts)')
def _():
    _p, s, _e, _r, g = range_case('base_wrap')
    eq((g['status'], g['join_usable'], g['range']),
       ('complete', False, dict(raw_base=0xFFFFF000, adjusted_base=0xFFFFF000, extent_start=0xFFFFF000,
                                extent_length=1888256, extent_end_exclusive=1884160, invariants_hold=False)), 'wrap')
    eq((s['guest_destination_ranges']['complete'], s['guest_destination_ranges']['join_usable']), (1, 0),
       'complete but not usable')


@test('T2B3 raw vs derived: declared pitch/height 1283x717 align to 1312x736 (texture_address.h L65-L67) and the tiled extent (1933312 B) is not the declared linear span (3679644 B); the raw base is the origin only because the rectangle starts at (8,0)')
def _():
    _p, _s, _e, r, g = range_case('unaligned_1283x717')
    eq((r['dest_pitch_decoded']['pitch'], r['dest_pitch_decoded']['height']), (1283, 717), 'raw /2 decode kept')
    d = g['destination']
    eq((d['pitch_aligned'], d['height_aligned'], d['pitch_aligned_div_32'], d['height_aligned_div_32']), (1312, 736, 41, 23),
       'aligned')
    eq(g['raw_vs_derived'], dict(adjusted_base_equals_raw=True, extent_start_equals_raw_base=True, adjusted_minus_raw=0,
                                 extent_start_minus_raw=0, declared_linear_span_bytes=3679644,
                                 extent_equals_declared_linear_span=False), 'raw vs derived')
    eq(g['range']['extent_length'], 1933312, 'extent')


@test('T2B census conservation: ten rows of all seven statuses in one trace; each counted once, sum == total == resolves == resolve_inputs.total; join_usable and the three raw-vs-derived counters exact; every resolve_details row carries its own derived object in order')
def _():
    p, _offs = range_trace('t2b_census', range_rows([
        ('base_A', 'inside', None), ('msaa3', 'inside', None), ('empty', 'inside', None), ('cmd2', 'inside', None),
        ('fmt0', 'inside', None), ('base_A', 'none', None), ('base_A', 'inside', dict(type=2)),
        ('scissor_cut_600_300', 'inside', None), ('base_wrap', 'inside', None), ('unaligned_1283x717', 'inside', None)]))
    s, _eng = summary(p)
    gr = s['guest_destination_ranges']
    eq(gr, dict(complete=4, rejected_copy_command=1, inputs_rejected=1, inputs_incomplete=1, rejected_msaa=1,
                empty_rectangle=1, non_resolvable_format=1, total=10, join_usable=3, adjusted_base_differs_from_raw=1,
                extent_start_differs_from_raw_base=1, extent_differs_from_declared_linear_span=5), 'census')
    eq((sum(gr[k] for k in X.RANGE_STATUSES), s['resolves'], s['resolve_inputs']['total']), (10, 10, 10), 'conserved')
    eq([r['guest_destination_range']['status'] for r in s['resolve_details']],
       ['complete', 'rejected_msaa', 'empty_rectangle', 'rejected_copy_command', 'non_resolvable_format',
        'inputs_incomplete', 'inputs_rejected', 'complete', 'complete', 'complete'], 'per-row statuses in order')
    eq([r['guest_destination_range']['join_usable'] for r in s['resolve_details']],
       [True, False, False, False, False, False, False, True, False, True], 'join_usable per row')
    eq((s['resolve_inputs']['complete'], s['resolve_inputs']['rejected'], s['resolve_inputs']['incomplete']), (8, 1, 1),
       '/3 block unchanged')
    for r in s['resolve_details']:
        for k in ('draw_ord', 'frame', 'copy_control', 'copy', 'dest_base', 'dest_pitch', 'dest_pitch_decoded', 'dest_info',
                  'dest', 'surface_info', 'rt_info', 'depth_info', 'source', 'get_resolve_info_inputs'):
            ck(k in r, '/3 row key %s preserved' % k)


@test('T2B JSON/human wiring: schema /4 and tool version, derivation_manifest and guest_destination_ranges in the file, resolve_details rows carry guest_destination_range through the JSON round trip, --resolves prints range=[...] per row (INVARIANTS-VIOLATED flagged) and the summary prints the ranges line')
def _():
    p, _offs = range_trace('t2b_json', range_rows([('base_A', 'inside', None), ('msaa3', 'inside', None),
                                                   ('base_wrap', 'inside', None)]))
    s, eng = summary(p)
    out = os.path.join(TMP, 't2b.json')
    rc, so, se = run_main([p, '--json', out, '--resolves'])
    eq(rc, 0, 'exit; stderr=%r' % se)
    j = json.load(open(out, encoding='utf-8'))
    eq((j['schema'], j['tool_version']), ('xtr_draws-summary/7', X.TOOL_VERSION), 'schema /7 (bumped by TOOL-3C)')
    eq(j['derivation_manifest'], s['derivation_manifest'], 'manifest in the file')
    eq(j['guest_destination_ranges'], s['guest_destination_ranges'], 'census in the file')
    eq([r['guest_destination_range'] for r in j['resolve_details']],
       [json.loads(json.dumps(r['guest_destination_range'])) for r in eng.resolves], 'rows round-trip')
    ck('range=[complete rect=(8,0)-(648,368) k_8_8_8_8(6) bpp=32 2D base=1F130000->1F130000 '
       'extent=[1F130000,1F2FD000) len=1888256]' in so, 'row 1 label: %r' % so[-1500:])
    ck('range=[rejected_msaa]' in so, 'row 2 label')
    ck('range=[complete rect=(8,0)-(648,368) k_8_8_8_8(6) bpp=32 2D base=FFFFF000->FFFFF000 '
       'extent=[FFFFF000,001CC000) len=1888256 INVARIANTS-VIOLATED]' in so, 'row 3 label')
    ck('ranges  : complete=2 joinUsable=1 emptyRect=0 nonResolvable=0 rejCmd=0 rejMsaa=1 inputsRejected=0 '
       'inputsIncomplete=0 of 3' in so, 'summary line: %r' % so[:1500])
    ck('derivation=xtr_draws-resolve-range/1' in so, 'derivation version printed')


@test('T2B provenance manifest: one top-level manifest (derivation version, repository, full 40-char pin, /3 input contract, EMPTY host-parameter dependency list, the six host parameters named unknown, integer model) with 23 stages of full-SHA line-range permalinks each cited once; rows carry only the version')
def _():
    _p, s, _e, _r, g = range_case('base_A')
    m = s['derivation_manifest']
    eq(list(m), ['derivation_version', 'xenia_repository', 'xenia_commit', 'input_schema', 'output',
                 'guest_range_host_parameter_dependencies', 'host_parameters_not_in_trace', 'host_parameter_note',
                 'integer_model', 'stages'], 'manifest keys')
    eq((m['derivation_version'], m['xenia_repository'], m['xenia_commit'], m['input_schema']),
       ('xtr_draws-resolve-range/1', 'https://github.com/xenia-canary/xenia-canary',
        '0b4e6c440e91f118b42a5e9e32a6b629ce4c387c', 'xtr_draws-summary/3'), 'identity')
    eq(m['guest_range_host_parameter_dependencies'], [], 'the range depends on NO host parameter')
    eq(m['host_parameters_not_in_trace'], list(X.RESOLVE_INPUTS_NOT_IN_TRACE), 'the six host parameters stay unknown')
    eq(len(m['host_parameters_not_in_trace']), 6, 'six')
    ck('L1076-L1079' in m['host_parameter_note'] and 'L1272-L1281' in m['host_parameter_note'], 'note cites the host paths')
    ck('-fwrapv' in m['integer_model'] and 'int64' in m['integer_model'], 'integer model stated')
    eq(list(m['stages']), ['copy_command_validation', 'vertex_fetch_validation', 'vertex_read', 'vertex_endian_swap',
                           'pixel_center_half_pixel', 'float_to_fixed_16_8', 'rectangle_min_max_top_left', 'window_offset',
                           'scissor', 'alignment_8', 'msaa_validation', 'surface_pitch_clamp', 'height_clamp',
                           'empty_region', 'div_8', 'destination_format', 'format_table', 'pitch_height_alignment_32',
                           'base_granularity', 'tiled_2d', 'tiled_3d', 'non_resolvable_format', 'extent_invariants'],
       'stages in arithmetic order')
    links = [u for us in m['stages'].values() for u in us]
    prefix = 'https://github.com/xenia-canary/xenia-canary/blob/0b4e6c440e91f118b42a5e9e32a6b629ce4c387c/src/xenia/'
    for u in links:
        ck(u.startswith(prefix) and '#L' in u and u.split('#L')[1].replace('-L', '').isdigit(), 'permalink form: %s' % u)
    eq(len(links), len(set(links)), 'no citation repeated across stages')
    ck(all(len(us) >= 1 for us in m['stages'].values()), 'every stage cited')
    ck(any(u.endswith('draw_util.cc#L939-L947') for u in links), 'GetResolveInfo entry cited')
    eq(g['derivation_version'], m['derivation_version'], 'row carries the version only')
    ck('github.com' not in json.dumps(g), 'row repeats no citation')


@test('T2B2 FloatToD3D11Fixed16p8 unit edges (graphics_util.cc L17-L60): NaN/tiny -> 0, saturation at 32768-1/256 and -32768 (32768-1/512 would round to 0x800000 without the guard), nearest-even ties of both signs, arithmetic shift for negatives')
def _():
    F = X.float_to_fixed16p8
    cases = [(float('nan'), 0), (float('inf'), 8388607), (float('-inf'), -8388608), (0.0, 0), (-0.0, 0),
             (1 / 513.0, 0), (1 / 512.0, 0), (3 / 512.0, 2), (5 / 512.0, 2), (7 / 512.0, 4), (0.5, 128), (-0.5, -128),
             (1.0, 256), (-1.0, -256), (32767.99609375, 8388607), (32767.9921875, 8388606), (40000.0, 8388607),
             (32767.998046875, 8388607), (-32767.998046875, -8388608),
             (-32768.0, -8388608), (-32767.99609375, -8388607), (-40000.0, -8388608), (8.001953125, 2048),
             (8.005859375, 2050), (8.0029296875, 2049), (-8.001953125, -2048), (-8.005859375, -2050),
             (-8.0029296875, -2049)]
    for f, want in cases:
        eq(F(f), want, 'fixed16p8(%r)' % f)


@test('T2B2 GpuSwap, single-precision addition, align, log2_floor, sign extension and GetScissor<false> unit vectors (xenos.h L1092-L1109; base/math.h L63-L65, L317-L323; draw_util.cc L636-L724)')
def _():
    eq([X.gpu_swap32(0x11223344, e) for e in range(4)], [0x11223344, 0x22114433, 0x44332211, 0x33441122],
       'GpuSwap kNone/k8in16/k8in32/k16in32')
    eq(X.f32_add(16383.5029296875, 0.5), 16384.00390625, 'float+float rounds once to single precision (tie to even)')
    eq(X.f32_add(1.0, 0.5), 1.5, 'plain')
    eq([X.xe_align(v, 32) for v in (0, 1, 1280, 1283, 717)], [0, 32, 1280, 1312, 736], 'align 32')
    eq([X.xe_align(v, 8) for v in (0, 1, 8, 9, 648, 16383)], [0, 8, 8, 16, 648, 16384], 'align 8')
    eq([X.log2_floor(v) for v in (1, 2, 3, 4, 8, 12, 16)], [0, 1, 1, 2, 3, 3, 4], 'log2_floor')
    eq([X.sext(v, 15) for v in (0, 0x3FFF, 0x4000, 0x7FF0, 0x7FFF)], [0, 16383, -16384, -16, -1], 'sext15')
    S = X.get_scissor_no_pitch_clamp
    eq(S(0, 0x02D00500, 0x00040008, 0, 0x20002000), dict(offset=[8, 4], extent=[1280, 720]),
       'window scissor shifted by the window offset')
    eq(S(0x80000000, 0x02D00500, 0x00040008, 0, 0x20002000), dict(offset=[0, 0], extent=[1280, 720]),
       'window_offset_disable (bit 31)')
    eq(S(0x40004000, 0x41404140, 0, 0, 0x20002000), dict(offset=[0, 0], extent=[320, 320]),
       '14-bit fields: padding bits 14/30 ignored')
    eq(S(0, 0x02D00500, 0, 0x7FF07FF0, 0x00640064), dict(offset=[0, 0], extent=[100, 100]),
       'negative screen scissor tl clamps to 0; br cuts')
    eq(S(0x012C0258, 0x019002BC, 0, 0, 0x00640064), dict(offset=[600, 300], extent=[0, 0]),
       'br below tl -> zero extent, never negative')
    eq(S(0, 0x02D00500, 0x00107FF0, 0, 0x20002000), dict(offset=[0, 16], extent=[1264, 720]), 'negative window offset x')
    eq(S(0, 0x02D00500, 0x00107FF0, 0x7FF07FF0, 0x20002000), dict(offset=[0, 16], extent=[1264, 720]),
       'a negative tl surviving the screen scissor is clamped to 0 (L713-L716)')


@test('T2B2 tiled address unit vectors (texture_address.h L172-L238; texture_util.h L294-L312; texture_util.cc L450-L510) and the pinned tables (64 formats, 26 kResolvable, depth substitution, kMaxResolveSize)')
def _():
    eq(X.tiled_2d(0, 0, 1280, 2), 0, 'origin')
    eq(X.tiled_lower_bound_2d(8, 0, 1280, 2), 0, 'lower bound floors to the 32-tile')
    eq(X.tiled_lower_bound_2d(576, 288, 1280, 2), 1548288, 'scissor-cut origin (576,288)')
    eq(X.tiled_upper_bound_2d(648, 368, 1280, 2), 1888256, 'upper bound (base extent)')
    eq((X.tiled_upper_bound_2d(0, 368, 1280, 2), X.tiled_upper_bound_2d(648, 0, 1280, 2)), (0, 0), 'zero right/bottom -> 0')
    eq(X.tiled_lower_bound_3d(16, 16, 3, 1280, 736, 2), 5120, '3D lower bound, slice 3 (front & ~4 = 3)')
    eq(X.tiled_upper_bound_3d(656, 384, 4, 1280, 736, 2), 529047552 - 0x1F130000, '3D upper bound')
    eq(X.tiled_upper_bound_3d(656, 384, 0, 1280, 736, 2), 0, 'zero back -> 0')
    eq(len(X.FORMAT_TABLE), 64, '64 formats')
    eq(sum(1 for _n, t, _b in X.FORMAT_TABLE if t == 'kResolvable'), 26, '26 resolvable (texture_info_formats.inl)')
    eq((X.FORMAT_TABLE[0], X.FORMAT_TABLE[2], X.FORMAT_TABLE[6], X.FORMAT_TABLE[22], X.FORMAT_TABLE[23], X.FORMAT_TABLE[24],
        X.FORMAT_TABLE[26], X.FORMAT_TABLE[38], X.FORMAT_TABLE[57], X.FORMAT_TABLE[63]),
       (('k_1_REVERSE', 'kUncompressed', 1), ('k_8', 'kResolvable', 8), ('k_8_8_8_8', 'kResolvable', 32),
        ('k_24_8', 'kUncompressed', 32), ('k_24_8_FLOAT', 'kUncompressed', 32), ('k_16', 'kResolvable', 16),
        ('k_16_16_16_16', 'kResolvable', 64), ('k_32_32_32_32_FLOAT', 'kResolvable', 128),
        ('k_32_32_32_FLOAT', 'kUncompressed', 96), ('k_2_10_10_10_FLOAT_EDRAM', 'kUncompressed', 32)), 'spot rows')
    eq((X.DEPTH_TEXTURE_FORMAT, X.MAX_RESOLVE_SIZE, X.STORAGE_ALIGN_BLOCKS, X.TEXTURE_TILE_WH, X.TEXTURE_TILE_DEPTH),
       ({0: 22, 1: 23}, 16376, 32, 32, 4), 'constants')
    eq(X.RANGE_STATUSES, ('complete', 'rejected_copy_command', 'inputs_rejected', 'inputs_incomplete', 'rejected_msaa',
                          'empty_rectangle', 'non_resolvable_format'), 'statuses')
    eq((X.SCHEMA, X.DERIVATION_VERSION), ('xtr_draws-summary/7', 'xtr_draws-resolve-range/1'), 'schema (bumped by TOOL-3C) and derivation version')


# ── TOOL-3A: static texture bindings (accepted /5 semantics, rebuilt from /6 by the expander) ─────────────────────────────
# Three sources of truth: (1) the PINNED-HEADER oracle archive/tool3a/oracle.cpp
# (compiled with -I reference/xenia-canary/src so it decodes with Xenia's own
# ucode.h / xenos.h bitfield structs; source SHA-256 8cfb227f1997668e558badccd2d04166e2b3388927fb3b6c1d278f82481f5e27, archived exe
# 60ae9c5a8a7e13aa901d7fce761d6831ba0f367bdd9f4b5926793d73a883c1de) whose outputs are retained verbatim below for 80 programs and
# 66 + 192 fetch-constant vectors; (2) hand-derived field placements from the
# pinned struct declarations; (3) synthetic traces exercising the wiring from
# shader load and fetch constants to rows, census and CLI.

TEXTURE_FETCH_FIELD_ORDER = ['type', 'sign_x', 'sign_y', 'sign_z', 'sign_w', 'clamp_x', 'clamp_y', 'clamp_z', 'pitch', 'tiled', 'format', 'endianness', 'request_size', 'stacked', 'nearest_clamp_policy', 'base_address', 'size_1d.width', 'size_2d.width', 'size_2d.height', 'size_2d.stack_depth', 'size_3d.width', 'size_3d.height', 'size_3d.depth', 'num_format', 'swizzle', 'exp_adjust', 'mag_filter', 'min_filter', 'mip_filter', 'aniso_filter', 'arbitrary_filter', 'border_size', 'vol_mag_filter', 'vol_min_filter', 'mip_min_level', 'mip_max_level', 'mag_aniso_walk', 'min_aniso_walk', 'lod_bias', 'grad_exp_adjust_h', 'grad_exp_adjust_v', 'border_color', 'force_bc_w_to_max', 'tri_clamp', 'aniso_bias', 'dimension', 'packed_mips', 'mip_address']

# (label, ucode dwords, oracle `walk` lines)
SHADER_ORACLE_VECTORS = [
    ('exec_family_1', [0x00012001, 0x00001000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x2265B1F5, 0x91B7584A, 0xD8F16ADF],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_2', [0x00012001, 0x00002000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0xF4BEA973, 0xDCF4BB99, 0xF2A4D27B],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_3', [0x00012001, 0x00003000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x3CEB3FFD, 0x97B75092, 0x8B529B4A],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=3 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_4', [0x00012001, 0x00004000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x3C6DA5D7, 0x4DA4F9FC, 0x1A6916C7],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=4 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_5', [0x00012001, 0x00005000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x9F767C45, 0x4164D839, 0xBDE5C099],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=5 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_6', [0x00012001, 0x00006000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0xCB1855FE, 0x92E5DFE8, 0xD26B9496],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=6 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_13', [0x00012001, 0x0000D000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x424E617B, 0x4A6F188A, 0xAF6D114C],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=13 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('exec_family_14', [0x00012001, 0x0000E000, 0x00000000, 0x00301001, 0x00000E88, 0x00004000, 0x1B591D75, 0x9DAA37E5, 0xB3DCA50A],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 1 vfetch 0',
      'ins cf=0 cfop=14 slot=1 op=1 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000008']),
    ('non_exec_cf_0', [0x00052001, 0x10010000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_7', [0x00052001, 0x10017000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_8', [0x00052001, 0x10018000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_9', [0x00052001, 0x10019000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_10', [0x00052001, 0x1001A000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_11', [0x00052001, 0x1001B000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_12', [0x00052001, 0x1001C000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('non_exec_cf_15', [0x00052001, 0x1001F000, 0x20000001, 0x00401001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=1 cfop=2 slot=1 op=1 const=4 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000010']),
    ('sequence_00', [0x00004001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0xD82C07CD, 0x629F6FBE, 0xC2094CAC, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 0 vfetch 0',
      'mask 00000000']),
    ('sequence_55', [0x00554001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0x33183EDB, 0xB388D258, 0x9D741646, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 4 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=2 op=1 const=2 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=3 op=27 const=17 dim=0 dst=3 dstswz=258 pred=1 cond=1 writes=1 binding=U',
      'ins cf=0 cfop=2 slot=4 op=24 const=5 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000006']),
    ('sequence_FF', [0x00FF4001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0x0E280585, 0x530C7F50, 0xA3F7580B, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 4 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=2 op=1 const=2 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=3 op=5 const=2 dim=1 dst=0 dstswz=F50 pred=0 cond=1 writes=1 binding=U',
      'ins cf=0 cfop=2 slot=4 op=24 const=5 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000006']),
    ('sequence_11', [0x00114001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0x85A0BCC1, 0x6A06E9AB, 0xCE834960, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 2 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=3 op=1 const=26 dim=1 dst=11 dstswz=9AB pred=0 cond=1 writes=1 binding=1',
      'mask 04000002']),
    ('sequence_AA', [0x00AA4001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0xC99322A5, 0x58512321, 0xE8EFA937, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 0 vfetch 0',
      'mask 00000000']),
    ('sequence_44', [0x00444001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0xBDDE03F1, 0x7763C84E, 0xF0D885E3, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 2 vfetch 0',
      'ins cf=0 cfop=2 slot=2 op=1 const=2 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=4 op=24 const=5 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000004']),
    ('sequence_33', [0x00334001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x0000C000, 0x3E572E0F, 0xD6A74DBE, 0x8090C4BD, 0x00501018, 0x00000E88, 0x00004000],
     ['dwords 15 pairs 5 bound 1 exec 1 slots 4 fetch 2 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=2 slot=3 op=15 const=5 dim=3 dst=50 dstswz=DBE pred=1 cond=1 writes=0 binding=U',
      'mask 00000002']),
    ('fetch_opcode_0', [0x00011001, 0x00002000, 0x00000000, 0x00001000, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 1',
      'mask 00000000']),
    ('fetch_opcode_1', [0x00011001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000002']),
    ('fetch_opcode_2', [0x00011001, 0x00002000, 0x00000000, 0x00201002, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=2 const=2 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_3', [0x00011001, 0x00002000, 0x00000000, 0x00301003, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=3 const=3 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_4', [0x00011001, 0x00002000, 0x00000000, 0x00401004, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=4 const=4 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_5', [0x00011001, 0x00002000, 0x00000000, 0x00501005, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=5 const=5 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_6', [0x00011001, 0x00002000, 0x00000000, 0x00601006, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=6 const=6 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_7', [0x00011001, 0x00002000, 0x00000000, 0x00701007, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=7 const=7 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_8', [0x00011001, 0x00002000, 0x00000000, 0x00801008, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=8 const=8 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_9', [0x00011001, 0x00002000, 0x00000000, 0x00901009, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=9 const=9 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_10', [0x00011001, 0x00002000, 0x00000000, 0x00A0100A, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=10 const=10 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_11', [0x00011001, 0x00002000, 0x00000000, 0x00B0100B, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=11 const=11 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_12', [0x00011001, 0x00002000, 0x00000000, 0x00C0100C, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=12 const=12 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_13', [0x00011001, 0x00002000, 0x00000000, 0x00D0100D, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=13 const=13 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_14', [0x00011001, 0x00002000, 0x00000000, 0x00E0100E, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=14 const=14 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_15', [0x00011001, 0x00002000, 0x00000000, 0x00F0100F, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=15 const=15 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_16', [0x00011001, 0x00002000, 0x00000000, 0x01001010, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=16 const=16 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00010000']),
    ('fetch_opcode_17', [0x00011001, 0x00002000, 0x00000000, 0x01101011, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=17 const=17 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00020000']),
    ('fetch_opcode_18', [0x00011001, 0x00002000, 0x00000000, 0x01201012, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=18 const=18 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00040000']),
    ('fetch_opcode_19', [0x00011001, 0x00002000, 0x00000000, 0x01301013, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=19 const=19 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00080000']),
    ('fetch_opcode_20', [0x00011001, 0x00002000, 0x00000000, 0x01401014, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=20 const=20 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_21', [0x00011001, 0x00002000, 0x00000000, 0x01501015, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=21 const=21 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_22', [0x00011001, 0x00002000, 0x00000000, 0x01601016, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=22 const=22 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_23', [0x00011001, 0x00002000, 0x00000000, 0x01701017, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=23 const=23 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_24', [0x00011001, 0x00002000, 0x00000000, 0x01801018, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=24 const=24 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000000']),
    ('fetch_opcode_25', [0x00011001, 0x00002000, 0x00000000, 0x01901019, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=25 const=25 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000000']),
    ('fetch_opcode_26', [0x00011001, 0x00002000, 0x00000000, 0x01A0101A, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=26 const=26 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000000']),
    ('fetch_opcode_27', [0x00011001, 0x00002000, 0x00000000, 0x01B0101B, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=27 const=27 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_28', [0x00011001, 0x00002000, 0x00000000, 0x01C0101C, 0x00000E88, 0x00000000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=28 const=28 dim=0 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_29', [0x00011001, 0x00002000, 0x00000000, 0x01D0101D, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=29 const=29 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_30', [0x00011001, 0x00002000, 0x00000000, 0x01E0101E, 0x00000E88, 0x00008000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=30 const=30 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('fetch_opcode_31', [0x00011001, 0x00002000, 0x00000000, 0x01F0101F, 0x00000E88, 0x0000C000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=31 const=31 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=U',
      'mask 00000000']),
    ('op1_dim0_swz000', [0x00011001, 0x10021000, 0x20000001, 0x00701001, 0x00000000, 0x00000000, 0x00701001, 0x80000000, 0x80000000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=1 const=7 dim=0 dst=1 dstswz=000 pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=2 op=1 const=7 dim=0 dst=1 dstswz=000 pred=1 cond=1 writes=1 binding=1',
      'mask 00000080']),
    ('op16_dim1_swzE88', [0x00011001, 0x10021000, 0x20000001, 0x00701010, 0x00000E88, 0x00004000, 0x00701010, 0x80000E88, 0x80004000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=16 const=7 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=2 op=16 const=7 dim=1 dst=1 dstswz=E88 pred=1 cond=1 writes=1 binding=1',
      'mask 00000080']),
    ('op17_dim2_swzFFF', [0x00011001, 0x10021000, 0x20000001, 0x00701011, 0x00000FFF, 0x00008000, 0x00701011, 0x80000FFF, 0x80008000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=17 const=7 dim=2 dst=1 dstswz=FFF pred=0 cond=0 writes=0 binding=1',
      'ins cf=1 cfop=2 slot=2 op=17 const=7 dim=2 dst=1 dstswz=FFF pred=1 cond=1 writes=0 binding=1',
      'mask 00000080']),
    ('op18_dim3_swzE88', [0x00011001, 0x10021000, 0x20000001, 0x00701012, 0x00000E88, 0x0000C000, 0x00701012, 0x80000E88, 0x8000C000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=18 const=7 dim=3 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=2 op=18 const=7 dim=3 dst=1 dstswz=E88 pred=1 cond=1 writes=1 binding=1',
      'mask 00000080']),
    ('op19_dim0_swzFFB', [0x00011001, 0x10021000, 0x20000001, 0x00701013, 0x00000FFB, 0x00000000, 0x00701013, 0x80000FFB, 0x80000000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=19 const=7 dim=0 dst=1 dstswz=FFB pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=2 op=19 const=7 dim=0 dst=1 dstswz=FFB pred=1 cond=1 writes=1 binding=1',
      'mask 00000080']),
    ('op1_dim3_swzFFF', [0x00011001, 0x10021000, 0x20000001, 0x00701001, 0x00000FFF, 0x0000C000, 0x00701001, 0x80000FFF, 0x8000C000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=1 const=7 dim=3 dst=1 dstswz=FFF pred=0 cond=0 writes=0 binding=1',
      'ins cf=1 cfop=2 slot=2 op=1 const=7 dim=3 dst=1 dstswz=FFF pred=1 cond=1 writes=0 binding=1',
      'mask 00000080']),
    ('op18_dim0_swzFFF', [0x00011001, 0x10021000, 0x20000001, 0x00701012, 0x00000FFF, 0x00000000, 0x00701012, 0x80000FFF, 0x80000000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=18 const=7 dim=0 dst=1 dstswz=FFF pred=0 cond=0 writes=0 binding=1',
      'ins cf=1 cfop=2 slot=2 op=18 const=7 dim=0 dst=1 dstswz=FFF pred=1 cond=1 writes=0 binding=1',
      'mask 00000080']),
    ('op19_dim3_swz000', [0x00011001, 0x10021000, 0x20000001, 0x00701013, 0x00000000, 0x0000C000, 0x00701013, 0x80000000, 0x8000C000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 0',
      'ins cf=0 cfop=1 slot=1 op=19 const=7 dim=3 dst=1 dstswz=000 pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=2 op=19 const=7 dim=3 dst=1 dstswz=000 pred=1 cond=1 writes=1 binding=1',
      'mask 00000080']),
    ('dup_slots', [0x00554001, 0x30051000, 0x20000015, 0x00201001, 0x00000E88, 0x00004000, 0x00201001, 0x00000E88, 0x00008000, 0x00901001, 0x00000E88, 0x00004000, 0x00201018, 0x00000E88, 0x00004000, 0x00081000, 0x00390E88, 0x00000003, 0x01F01001, 0x00000E88, 0x00004000, 0x01E01019, 0x00000E88, 0x00004000, 0x01D0101A, 0x00000E88, 0x00004000],
     ['dwords 27 pairs 9 bound 1 exec 2 slots 7 fetch 7 vfetch 1',
      'ins cf=0 cfop=1 slot=1 op=1 const=2 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=1 slot=2 op=1 const=2 dim=2 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=1 slot=3 op=1 const=9 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=0 cfop=1 slot=4 op=24 const=2 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'ins cf=1 cfop=2 slot=6 op=1 const=31 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'ins cf=1 cfop=2 slot=7 op=25 const=30 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 80000204']),
    ('vertex_only', [0x00052001, 0x00002000, 0x00000000, 0x00081000, 0x00390E88, 0x00000003, 0x02181000, 0x00390E88, 0x00000003],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 2 fetch 2 vfetch 2',
      'mask 00000000']),
    ('setters_only', [0x00153001, 0x00002000, 0x00000000, 0x00101018, 0x00000E88, 0x00004000, 0x00201019, 0x00000E88, 0x00004000, 0x0030101A, 0x00000E88, 0x00004000],
     ['dwords 12 pairs 4 bound 1 exec 1 slots 3 fetch 3 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=24 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'ins cf=0 cfop=2 slot=2 op=25 const=2 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'ins cf=0 cfop=2 slot=3 op=26 const=3 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=0',
      'mask 00000000']),
    ('bound_shrinks', [0x00011002, 0x10011000, 0x20000001, 0x00011000, 0x00002000, 0x00000000, 0x00601001, 0x00000E88, 0x00004000],
     ['dwords 9 pairs 3 bound 1 exec 2 slots 2 fetch 2 vfetch 1',
      'ins cf=0 cfop=1 slot=2 op=1 const=6 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000040']),
    ('bound_zero', [0x00011000, 0x00002000, 0x00000000, 0x00601001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 0 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('exec_beyond', [0x00011005, 0x00002000, 0x00000000],
     ['dwords 3 pairs 1 bound 1 exec 1 slots 0 fetch 0 vfetch 0 malformed 5',
      'mask 00000000']),
    ('exec_beyond_second', [0x00052001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0 malformed 2',
      'ins cf=0 cfop=2 slot=1 op=1 const=1 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000002']),
    ('count_zero', [0x00000001, 0x00002000, 0x00000000, 0x00101001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 1 exec 1 slots 0 fetch 0 vfetch 0',
      'mask 00000000']),
    ('no_exec', [0x00000000, 0x00000000, 0xC0000000, 0x00101001, 0x00000E88, 0x00004000],
     ['dwords 6 pairs 2 bound 2 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('empty', [],
     ['dwords 0 pairs 0 bound 0 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('one_dword', [0x12345678],
     ['dwords 1 pairs 0 bound 0 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('two_dwords', [0x12345678, 0x9ABCDEF0],
     ['dwords 2 pairs 0 bound 0 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('tail_dwords', [0x00011001, 0x00002000, 0x00000000, 0x00201001, 0x00000E88, 0x00004000, 0xDEADBEEF],
     ['dwords 7 pairs 2 bound 1 exec 1 slots 1 fetch 1 vfetch 0',
      'ins cf=0 cfop=2 slot=1 op=1 const=2 dim=1 dst=1 dstswz=E88 pred=0 cond=0 writes=1 binding=1',
      'mask 00000004']),
    ('random_000', [0x014E7001, 0x70049000, 0x600008F5, 0x34DD449A, 0xFF7C8199, 0xE9A187C1, 0x886923C4, 0x5BD9352D, 0xCC21FCEB, 0x11F391BB, 0x13280FD7, 0x800000C0, 0xEC61F29A, 0x8D976547, 0x00004188, 0x920AB7D3, 0x212DCFEF, 0x30446DAC],
     ['dwords 18 pairs 6 bound 4 exec 1 slots 2 fetch 2 vfetch 0 malformed 6',
      'ins cf=1 cfop=6 slot=4 op=26 const=6 dim=1 dst=31 dstswz=547 pred=1 cond=0 writes=1 binding=0',
      'ins cf=1 cfop=6 slot=5 op=19 const=0 dim=1 dst=43 dstswz=FEF pred=0 cond=0 writes=0 binding=1',
      'mask 00000001']),
    ('random_001', [0x0F3A5004, 0x2004D000, 0xC0000244, 0x0ACD3004, 0x30045000, 0x9000031C, 0x086C6004, 0x30047000, 0x10000951, 0x05E05000, 0x5004D000, 0x500002C4],
     ['dwords 12 pairs 4 bound 0 exec 0 slots 0 fetch 0 vfetch 0 noexec',
      'mask 00000000']),
    ('random_002', [0x055B2004, 0x7004E000, 0x2000092A, 0x0D5D6003, 0x4004A000, 0x400007A8, 0x0C983001, 0x1004D000, 0x1000036D],
     ['dwords 9 pairs 3 bound 1 exec 1 slots 0 fetch 0 vfetch 0 malformed 4',
      'mask 00000000']),
    ('random_003', [0x0A0B3003, 0x6003F000, 0x600009DC, 0x031A6003, 0x1003E000, 0xD0000C90, 0x0B465003, 0x7003A000, 0xC0000464],
     ['dwords 9 pairs 3 bound 3 exec 1 slots 0 fetch 0 vfetch 0 malformed 3',
      'mask 00000000']),
]

# (bit index 0..191 of the six dwords, [(field, value) the oracle reports for that bit alone])
FETCH_BIT_TABLE = [
    (0, [('type', 1)]),
    (1, [('type', 2)]),
    (2, [('sign_x', 1)]),
    (3, [('sign_x', 2)]),
    (4, [('sign_y', 1)]),
    (5, [('sign_y', 2)]),
    (6, [('sign_z', 1)]),
    (7, [('sign_z', 2)]),
    (8, [('sign_w', 1)]),
    (9, [('sign_w', 2)]),
    (10, [('clamp_x', 1)]),
    (11, [('clamp_x', 2)]),
    (12, [('clamp_x', 4)]),
    (13, [('clamp_y', 1)]),
    (14, [('clamp_y', 2)]),
    (15, [('clamp_y', 4)]),
    (16, [('clamp_z', 1)]),
    (17, [('clamp_z', 2)]),
    (18, [('clamp_z', 4)]),
    (19, []),
    (20, []),
    (21, []),
    (22, [('pitch', 1)]),
    (23, [('pitch', 2)]),
    (24, [('pitch', 4)]),
    (25, [('pitch', 8)]),
    (26, [('pitch', 16)]),
    (27, [('pitch', 32)]),
    (28, [('pitch', 64)]),
    (29, [('pitch', 128)]),
    (30, [('pitch', 256)]),
    (31, [('tiled', 1)]),
    (32, [('format', 1)]),
    (33, [('format', 2)]),
    (34, [('format', 4)]),
    (35, [('format', 8)]),
    (36, [('format', 16)]),
    (37, [('format', 32)]),
    (38, [('endianness', 1)]),
    (39, [('endianness', 2)]),
    (40, [('request_size', 1)]),
    (41, [('request_size', 2)]),
    (42, [('stacked', 1)]),
    (43, [('nearest_clamp_policy', 1)]),
    (44, [('base_address', 1)]),
    (45, [('base_address', 2)]),
    (46, [('base_address', 4)]),
    (47, [('base_address', 8)]),
    (48, [('base_address', 16)]),
    (49, [('base_address', 32)]),
    (50, [('base_address', 64)]),
    (51, [('base_address', 128)]),
    (52, [('base_address', 256)]),
    (53, [('base_address', 512)]),
    (54, [('base_address', 1024)]),
    (55, [('base_address', 2048)]),
    (56, [('base_address', 4096)]),
    (57, [('base_address', 8192)]),
    (58, [('base_address', 16384)]),
    (59, [('base_address', 32768)]),
    (60, [('base_address', 65536)]),
    (61, [('base_address', 131072)]),
    (62, [('base_address', 262144)]),
    (63, [('base_address', 524288)]),
    (64, [('size_1d.width', 1), ('size_2d.width', 1), ('size_3d.width', 1)]),
    (65, [('size_1d.width', 2), ('size_2d.width', 2), ('size_3d.width', 2)]),
    (66, [('size_1d.width', 4), ('size_2d.width', 4), ('size_3d.width', 4)]),
    (67, [('size_1d.width', 8), ('size_2d.width', 8), ('size_3d.width', 8)]),
    (68, [('size_1d.width', 16), ('size_2d.width', 16), ('size_3d.width', 16)]),
    (69, [('size_1d.width', 32), ('size_2d.width', 32), ('size_3d.width', 32)]),
    (70, [('size_1d.width', 64), ('size_2d.width', 64), ('size_3d.width', 64)]),
    (71, [('size_1d.width', 128), ('size_2d.width', 128), ('size_3d.width', 128)]),
    (72, [('size_1d.width', 256), ('size_2d.width', 256), ('size_3d.width', 256)]),
    (73, [('size_1d.width', 512), ('size_2d.width', 512), ('size_3d.width', 512)]),
    (74, [('size_1d.width', 1024), ('size_2d.width', 1024), ('size_3d.width', 1024)]),
    (75, [('size_1d.width', 2048), ('size_2d.width', 2048), ('size_3d.height', 1)]),
    (76, [('size_1d.width', 4096), ('size_2d.width', 4096), ('size_3d.height', 2)]),
    (77, [('size_1d.width', 8192), ('size_2d.height', 1), ('size_3d.height', 4)]),
    (78, [('size_1d.width', 16384), ('size_2d.height', 2), ('size_3d.height', 8)]),
    (79, [('size_1d.width', 32768), ('size_2d.height', 4), ('size_3d.height', 16)]),
    (80, [('size_1d.width', 65536), ('size_2d.height', 8), ('size_3d.height', 32)]),
    (81, [('size_1d.width', 131072), ('size_2d.height', 16), ('size_3d.height', 64)]),
    (82, [('size_1d.width', 262144), ('size_2d.height', 32), ('size_3d.height', 128)]),
    (83, [('size_1d.width', 524288), ('size_2d.height', 64), ('size_3d.height', 256)]),
    (84, [('size_1d.width', 1048576), ('size_2d.height', 128), ('size_3d.height', 512)]),
    (85, [('size_1d.width', 2097152), ('size_2d.height', 256), ('size_3d.height', 1024)]),
    (86, [('size_1d.width', 4194304), ('size_2d.height', 512), ('size_3d.depth', 1)]),
    (87, [('size_1d.width', 8388608), ('size_2d.height', 1024), ('size_3d.depth', 2)]),
    (88, [('size_2d.height', 2048), ('size_3d.depth', 4)]),
    (89, [('size_2d.height', 4096), ('size_3d.depth', 8)]),
    (90, [('size_2d.stack_depth', 1), ('size_3d.depth', 16)]),
    (91, [('size_2d.stack_depth', 2), ('size_3d.depth', 32)]),
    (92, [('size_2d.stack_depth', 4), ('size_3d.depth', 64)]),
    (93, [('size_2d.stack_depth', 8), ('size_3d.depth', 128)]),
    (94, [('size_2d.stack_depth', 16), ('size_3d.depth', 256)]),
    (95, [('size_2d.stack_depth', 32), ('size_3d.depth', 512)]),
    (96, [('num_format', 1)]),
    (97, [('swizzle', 1)]),
    (98, [('swizzle', 2)]),
    (99, [('swizzle', 4)]),
    (100, [('swizzle', 8)]),
    (101, [('swizzle', 16)]),
    (102, [('swizzle', 32)]),
    (103, [('swizzle', 64)]),
    (104, [('swizzle', 128)]),
    (105, [('swizzle', 256)]),
    (106, [('swizzle', 512)]),
    (107, [('swizzle', 1024)]),
    (108, [('swizzle', 2048)]),
    (109, [('exp_adjust', 1)]),
    (110, [('exp_adjust', 2)]),
    (111, [('exp_adjust', 4)]),
    (112, [('exp_adjust', 8)]),
    (113, [('exp_adjust', 16)]),
    (114, [('exp_adjust', -32)]),
    (115, [('mag_filter', 1)]),
    (116, [('mag_filter', 2)]),
    (117, [('min_filter', 1)]),
    (118, [('min_filter', 2)]),
    (119, [('mip_filter', 1)]),
    (120, [('mip_filter', 2)]),
    (121, [('aniso_filter', 1)]),
    (122, [('aniso_filter', 2)]),
    (123, [('aniso_filter', 4)]),
    (124, [('arbitrary_filter', 1)]),
    (125, [('arbitrary_filter', 2)]),
    (126, [('arbitrary_filter', 4)]),
    (127, [('border_size', 1)]),
    (128, [('vol_mag_filter', 1)]),
    (129, [('vol_min_filter', 1)]),
    (130, [('mip_min_level', 1)]),
    (131, [('mip_min_level', 2)]),
    (132, [('mip_min_level', 4)]),
    (133, [('mip_min_level', 8)]),
    (134, [('mip_max_level', 1)]),
    (135, [('mip_max_level', 2)]),
    (136, [('mip_max_level', 4)]),
    (137, [('mip_max_level', 8)]),
    (138, [('mag_aniso_walk', 1)]),
    (139, [('min_aniso_walk', 1)]),
    (140, [('lod_bias', 1)]),
    (141, [('lod_bias', 2)]),
    (142, [('lod_bias', 4)]),
    (143, [('lod_bias', 8)]),
    (144, [('lod_bias', 16)]),
    (145, [('lod_bias', 32)]),
    (146, [('lod_bias', 64)]),
    (147, [('lod_bias', 128)]),
    (148, [('lod_bias', 256)]),
    (149, [('lod_bias', -512)]),
    (150, [('grad_exp_adjust_h', 1)]),
    (151, [('grad_exp_adjust_h', 2)]),
    (152, [('grad_exp_adjust_h', 4)]),
    (153, [('grad_exp_adjust_h', 8)]),
    (154, [('grad_exp_adjust_h', -16)]),
    (155, [('grad_exp_adjust_v', 1)]),
    (156, [('grad_exp_adjust_v', 2)]),
    (157, [('grad_exp_adjust_v', 4)]),
    (158, [('grad_exp_adjust_v', 8)]),
    (159, [('grad_exp_adjust_v', -16)]),
    (160, [('border_color', 1)]),
    (161, [('border_color', 2)]),
    (162, [('force_bc_w_to_max', 1)]),
    (163, [('tri_clamp', 1)]),
    (164, [('tri_clamp', 2)]),
    (165, [('aniso_bias', 1)]),
    (166, [('aniso_bias', 2)]),
    (167, [('aniso_bias', 4)]),
    (168, [('aniso_bias', -8)]),
    (169, [('dimension', 1)]),
    (170, [('dimension', 2)]),
    (171, [('packed_mips', 1)]),
    (172, [('mip_address', 1)]),
    (173, [('mip_address', 2)]),
    (174, [('mip_address', 4)]),
    (175, [('mip_address', 8)]),
    (176, [('mip_address', 16)]),
    (177, [('mip_address', 32)]),
    (178, [('mip_address', 64)]),
    (179, [('mip_address', 128)]),
    (180, [('mip_address', 256)]),
    (181, [('mip_address', 512)]),
    (182, [('mip_address', 1024)]),
    (183, [('mip_address', 2048)]),
    (184, [('mip_address', 4096)]),
    (185, [('mip_address', 8192)]),
    (186, [('mip_address', 16384)]),
    (187, [('mip_address', 32768)]),
    (188, [('mip_address', 65536)]),
    (189, [('mip_address', 131072)]),
    (190, [('mip_address', 262144)]),
    (191, [('mip_address', 524288)]),
]

# (label, raw six dwords, the oracle's 48 fields in TEXTURE_FETCH_FIELD_ORDER)
FETCH_ORACLE_VECTORS = [
    ('all_ones', [0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF], [3, 3, 3, 3, 3, 7, 7, 7, 511, 1, 63, 3, 3, 1, 1, 1048575, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 1, 4095, -1, 3, 3, 3, 7, 7, 1, 1, 1, 15, 15, 1, 1, -1, -1, -1, 3, 1, 3, -1, 3, 1, 1048575]),
    ('zero', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_0', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_1', [0x00000000, 0x00000000, 0x00000000, 0x00002000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_31', [0x00000000, 0x00000000, 0x00000000, 0x0003E000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_32', [0x00000000, 0x00000000, 0x00000000, 0x00040000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_63', [0x00000000, 0x00000000, 0x00000000, 0x0007E000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('exp_adjust_62', [0x00000000, 0x00000000, 0x00000000, 0x0007C000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_0', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_1', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00001000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_511', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x001FF000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 511, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_512', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00200000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -512, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_1023', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x003FF000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('lod_bias_1022', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x003FE000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_0', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_1', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00400000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_15', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x03C00000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_16', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x04000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -16, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_31', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x07C00000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_h_30', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x07800000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_0', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_1', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x08000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_15', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x78000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_16', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -16, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_31', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xF8000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0]),
    ('grad_exp_adjust_v_30', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xF0000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, 0, 0, 0, 0, 0, 0]),
    ('aniso_bias_0', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('aniso_bias_1', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000020], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]),
    ('aniso_bias_7', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x000000E0], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0]),
    ('aniso_bias_8', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000100], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -8, 0, 0, 0]),
    ('aniso_bias_15', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x000001E0], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0]),
    ('aniso_bias_14', [0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x000001C0], [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -2, 0, 0, 0]),
    ('dim0_size00000000', [0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_size00FFFFFF', [0x00000002, 0x00000000, 0x00FFFFFF, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 2047, 0, 2047, 2047, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_size002CE27F', [0x00000002, 0x00000000, 0x002CE27F, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2941567, 639, 359, 0, 639, 1436, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_size07C3F87F', [0x00000002, 0x00000000, 0x07C3F87F, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12843135, 6271, 7711, 1, 127, 127, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim0_size000003FF', [0x00000002, 0x00000000, 0x000003FF, 0x00000000, 0x00000000, 0x00000000], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 1023, 0, 0, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ('dim1_size00000000', [0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_size00FFFFFF', [0x00000002, 0x00000000, 0x00FFFFFF, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 2047, 0, 2047, 2047, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_size002CE27F', [0x00000002, 0x00000000, 0x002CE27F, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2941567, 639, 359, 0, 639, 1436, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_size07C3F87F', [0x00000002, 0x00000000, 0x07C3F87F, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12843135, 6271, 7711, 1, 127, 127, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim1_size000003FF', [0x00000002, 0x00000000, 0x000003FF, 0x00000000, 0x00000000, 0x00000200], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 1023, 0, 0, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]),
    ('dim2_size00000000', [0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_size00FFFFFF', [0x00000002, 0x00000000, 0x00FFFFFF, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 2047, 0, 2047, 2047, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_size002CE27F', [0x00000002, 0x00000000, 0x002CE27F, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2941567, 639, 359, 0, 639, 1436, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_size07C3F87F', [0x00000002, 0x00000000, 0x07C3F87F, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12843135, 6271, 7711, 1, 127, 127, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim2_size000003FF', [0x00000002, 0x00000000, 0x000003FF, 0x00000000, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 1023, 0, 0, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('dim3_size00000000', [0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_size00FFFFFF', [0x00000002, 0x00000000, 0x00FFFFFF, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 2047, 0, 2047, 2047, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_sizeFFFFFFFF', [0x00000002, 0x00000000, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16777215, 8191, 8191, 63, 2047, 2047, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_size002CE27F', [0x00000002, 0x00000000, 0x002CE27F, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2941567, 639, 359, 0, 639, 1436, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_size07C3F87F', [0x00000002, 0x00000000, 0x07C3F87F, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12843135, 6271, 7711, 1, 127, 127, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('dim3_size000003FF', [0x00000002, 0x00000000, 0x000003FF, 0x00000000, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1023, 1023, 0, 0, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('title_like_2d', [0x80001D02, 0x1F900486, 0x002CE27F, 0x00A80D10, 0x003F00C0, 0x1FA00A00], [2, 0, 0, 0, 1, 7, 0, 0, 0, 1, 6, 2, 0, 1, 0, 129280, 2941567, 639, 359, 0, 639, 1436, 0, 0, 1672, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 3, 0, 0, -16, 0, 0, 0, 0, 0, 0, 1, 1, 129536]),
    ('cube_like', [0x80000002, 0x0BA5A006, 0x140FE07F, 0x00000D10, 0x00000000, 0x00000600], [2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 6, 0, 0, 0, 0, 47706, 1040511, 127, 127, 5, 127, 508, 80, 0, 1672, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0]),
    ('3d_like', [0x00000002, 0x00000013, 0x01C1F83F, 0x00000D10, 0x00000000, 0x00000400], [2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19, 0, 0, 0, 0, 0, 12711999, 6207, 3599, 0, 63, 63, 7, 0, 1672, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0]),
    ('random_0000', [0x18720192, 0x9F423A90, 0xE752C11E, 0xC8E04639, 0x3095AA56, 0xE82A9F5A], [2, 0, 1, 2, 1, 0, 0, 2, 97, 0, 16, 2, 2, 0, 1, 652323, 5423390, 286, 6806, 57, 286, 600, 925, 1, 796, 2, 0, 3, 1, 4, 4, 1, 0, 1, 5, 9, 0, 1, 346, 2, 6, 2, 0, 3, -6, 3, 1, 950953]),
    ('random_0001', [0x3B940E59, 0xD0542E96, 0xE4D88659, 0xD58E2B4F, 0xCA1B74D5, 0xCDF2610D], [1, 2, 1, 1, 2, 3, 0, 4, 238, 0, 22, 2, 2, 1, 1, 853314, 14190169, 1625, 1732, 57, 1625, 784, 915, 1, 1447, -15, 1, 0, 3, 2, 5, 1, 1, 0, 5, 3, 1, 0, 439, 8, -7, 1, 1, 1, -8, 0, 0, 843558]),
    ('random_0002', [0x5F4CA744, 0x60272F0D, 0x6BA2E813, 0x550F5D8E, 0x0D0054BA, 0x5D4C350F], [0, 1, 0, 1, 3, 1, 5, 4, 381, 0, 13, 0, 3, 1, 1, 393842, 10676243, 2067, 7447, 26, 19, 1117, 430, 0, 3783, -6, 1, 0, 2, 2, 5, 0, 0, 1, 14, 2, 1, 0, 5, -12, 1, 3, 1, 1, -8, 2, 0, 382147]),
]


def tfi(opcode=1, const=0, dim=1, dst=1, dst_swiz=0xE88, src=0, pred=0, cond=0, lod_bias=0, extra0=0, extra1=0):
    """ucode.h TextureFetchInstruction L835-L879 (independent transcription)."""
    d0 = opcode | (src << 5) | (dst << 12) | (const << 20) | extra0
    d1 = dst_swiz | (pred << 31) | extra1
    d2 = ((lod_bias & 0x7F) << 2) | (dim << 14) | (cond << 31)
    return [d0 & 0xFFFFFFFF, d1 & 0xFFFFFFFF, d2 & 0xFFFFFFFF]


def alu3(a=0x12345678, b=0x9ABC0000, c=0x0F0F0F0F):
    """ALU filler whose dwords decode as (cnop, cnop) if ever read as CF."""
    return [a, b, c]


def ps_prog(*instrs, op=2):
    """One exec-class CF naming every instruction as a FETCH slot, then the slots."""
    n = len(instrs)
    seq = sum(1 << (2 * k) for k in range(n))
    prog = cf_pair(cf(op, 1, n, seq))
    for i in instrs:
        prog += i
    return prog


def tfc(type_=2, fmt=6, dim=1, base=0x1F900000, w=640, h=360, d=1, pitch=20, tiled=1, endian=2, swz=0x688, mag=1, mn=1,
        mip=1, aniso=0, mipmin=0, mipmax=0, packed=0, mipbase=0, stacked=0, exp_adjust=0, lod_bias=0, sign_x=0,
        clamp_x=0, clamp_y=0, clamp_z=0):
    """xenos.h xe_gpu_texture_fetch_t L1229-L1330 (independent transcription; size views per dimension)."""
    d0 = type_ | (sign_x << 2) | (clamp_x << 10) | (clamp_y << 13) | (clamp_z << 16) | (pitch << 22) | (tiled << 31)
    d1 = fmt | (endian << 6) | (stacked << 10) | ((base >> 12) << 12)
    if dim == 0:
        d2 = w - 1
    elif dim == 2:
        d2 = (w - 1) | ((h - 1) << 11) | ((d - 1) << 22)
    else:
        d2 = (w - 1) | ((h - 1) << 13) | ((d - 1) << 26)
    d3 = (swz << 1) | ((exp_adjust & 0x3F) << 13) | (mag << 19) | (mn << 21) | (mip << 23) | (aniso << 25)
    d4 = (mipmin << 2) | (mipmax << 6) | ((lod_bias & 0x3FF) << 12)
    d5 = (dim << 9) | (packed << 11) | ((mipbase >> 12) << 12)
    return [x & 0xFFFFFFFF for x in (d0, d1, d2, d3, d4, d5)]


def binding_trace(name, vs=None, ps=None, consts=(), mode=4, draws=1):
    tr = base_trace(mode)
    if vs is not None:
        tr.packet(im_load_imm(0, vs))
    if ps is not None:
        tr.packet(im_load_imm(1, ps))
    for slot, dw in consts:
        tr.packet(set_const(1, slot * 6, *dw))
    for _ in range(draws):
        tr.packet(draw2())
    return tr.save(name)


PS_A = ps_prog(tfi(const=1), tfi(const=4, dim=2), tfi(opcode=24, const=1))
TFC_1 = tfc(fmt=20, dim=1, base=0x11060000, w=512, h=512, pitch=16, swz=0x688, mipmax=9, packed=1, mipbase=0x110A0000)
TFC_4 = tfc(fmt=6, dim=2, base=0x1F900000, w=64, h=64, d=8)
TFC_6 = tfc(fmt=2, dim=0, base=0x1FA00000, w=256)
CONSTS_A = [(1, TFC_1), (4, TFC_4), (6, TFC_6)]


def walk_lines(ucode):
    """The oracle's `walk` line format produced from the Python analysis."""
    a = X.analyze_texture_fetches(ucode)
    w = X.cf_exec_walk(ucode)
    head = 'dwords %d pairs %d bound %d exec %d slots %d fetch %d vfetch %d%s' % (
        w['dwords'], w['pairs'], w['bound'], w['exec_count'], len(w['slots']), a['fetch_slots'], a['vertex_fetch_slots'],
        '' if w['saw_exec'] else ' noexec')
    if w['malformed']:
        head += ' malformed %d' % w['malformed_slot']
    lines = [head]
    for ins in a['instructions']:
        lines.append('ins cf=%d cfop=%d slot=%d op=%d const=%d dim=%d dst=%d dstswz=%03X pred=%d cond=%d writes=%d binding=%s' % (
            ins['cf_index'], ins['cf_opcode'], ins['slot'], ins['opcode'], ins['fetch_constant'], ins['dimension'],
            ins['dst_reg'], ins['dst_swizzle'], ins['is_predicated'], ins['predicate_condition'], int(ins['writes_result']),
            'U' if not ins['opcode_known'] else ('1' if ins['is_binding'] else '0')))
    lines.append('mask %08X' % a['binding_mask'])
    return lines, a


@test('T3A2 fixed pinned-header oracle vectors: the shared CF walk + texture-fetch analysis reproduces the oracle line for line on 80 retained programs (every exec family, every non-exec CF opcode, sequence patterns, all 32 fetch opcodes, dimensions x swizzles, duplicates, bounds, malformed, empty, randoms)')
def _():
    eq(len(SHADER_ORACLE_VECTORS), 80, 'vector count')
    eq(len({l for l, _d, _o in SHADER_ORACLE_VECTORS}), 80, 'labels unique')
    statuses = set()
    for label, dwords, oracle in SHADER_ORACLE_VECTORS:
        lines, a = walk_lines(be_bytes(dwords))
        eq(lines, oracle, '%s: python vs oracle' % label)
        statuses.add(a['status'])
    eq(statuses, {'complete', 'incomplete'}, 'both statuses exercised')


@test('T3A3 fetch-constant bit isolation: each of the 192 bits of the six dwords lands in exactly the pinned field(s) with the oracle value (padding bits 19-21 of dword 0 land nowhere; dword 2 feeds all three size views; sign bits go negative)')
def _():
    eq(list(X.TEXTURE_FETCH_FIELD_ORDER), TEXTURE_FETCH_FIELD_ORDER, 'field order pinned (declaration order of xe_gpu_texture_fetch_t)')
    eq(len(FETCH_BIT_TABLE), 192, 'every bit')
    hit = set()
    for bit, fields in FETCH_BIT_TABLE:
        raw = [0] * 6
        raw[bit // 32] = 1 << (bit % 32)
        got = X.tfetch_field_vector(X.decode_tfetch(raw))
        exp = [0] * len(TEXTURE_FETCH_FIELD_ORDER)
        for name, val in fields:
            exp[TEXTURE_FETCH_FIELD_ORDER.index(name)] = val
            hit.add(name)
        eq(got, exp, 'bit %d (dword %d bit %d) -> %r' % (bit, bit // 32, bit % 32, fields))
    eq(sorted(set(TEXTURE_FETCH_FIELD_ORDER) - hit), [], 'every declared field is reached by some bit')
    eq([b for b, f in FETCH_BIT_TABLE if not f], [19, 20, 21], 'only the three padding bits of dword 0 land nowhere')
    neg = sorted({name for _b, fields in FETCH_BIT_TABLE for name, val in fields if val < 0})
    eq(neg, ['aniso_bias', 'exp_adjust', 'grad_exp_adjust_h', 'grad_exp_adjust_v', 'lod_bias'], 'the five signed fields')


@test('T3A3 fetch-constant full vectors: all-ones, zero, six boundary values of every signed field, seven size patterns x four dimensions, three realistic constants and randoms equal the oracle field for field')
def _():
    eq(len(FETCH_ORACLE_VECTORS), 66, 'vector count')
    for label, raw, fields in FETCH_ORACLE_VECTORS:
        eq(X.tfetch_field_vector(X.decode_tfetch(raw)), fields, '%s: python vs oracle' % label)
    t = X.decode_tfetch([0xFFFFFFFF] * 6)
    eq((t['exp_adjust'], t['lod_bias'], t['grad_exp_adjust_h'], t['grad_exp_adjust_v'], t['aniso_bias']), (-1, -1, -1, -1, -1),
       'all-ones signed fields are -1')


@test('decode_tfetch: every corrected dword placement by hand from xe_gpu_texture_fetch_t (swizzle and filters in dword 3, mip bounds and LOD bias in dword 4, dimension and mip page in dword 5), the size views per dimension and the byte scales')
def _():
    raw = [2 | (1 << 2) | (7 << 10) | (20 << 22) | (1 << 31), 0x1F900006 | (2 << 6) | (1 << 8) | (1 << 10), 639 | (359 << 13) | (5 << 26),
           (0x688 << 1) | (1 << 19) | (1 << 21) | (2 << 23) | (5 << 25) | (0x3E << 13) | (1 << 31) | 1,
           (3 << 6) | (2 << 2) | (0x3F0 << 12) | (0x10 << 22) | (0x0F << 27) | 3 | (1 << 10),
           (1 << 9) | (1 << 11) | (0x1FA00 << 12) | 2 | (1 << 2) | (3 << 3) | (0x8 << 5)]
    t = X.decode_tfetch(raw)
    eq((t['type'], t['sign_x'], t['clamp_x'], t['pitch'], t['pitch_pixels'], t['tiled']), (2, 1, 7, 20, 640, 1), 'dword 0')
    eq((t['format'], t['format_name'], t['endianness'], t['request_size'], t['stacked'], t['base_address'], t['base_address_bytes']),
       (6, 'k_8_8_8_8', 2, 1, 1, 0x1F900, 0x1F900000), 'dword 1')
    eq((t['size_2d']['width'], t['size_2d']['height'], t['size_2d']['stack_depth']), (639, 359, 5), 'dword 2 as 2D')
    eq((t['size_1d']['width'], t['size_3d']['width'], t['size_3d']['height'], t['size_3d']['depth']),
       ((639 | (359 << 13) | (5 << 26)) & 0xFFFFFF, 639 & 0x7FF, ((639 | (359 << 13)) >> 11) & 0x7FF, ((359 << 13) | (5 << 26)) >> 22), 'dword 2 other views')
    eq((t['num_format'], t['swizzle'], t['exp_adjust'], t['mag_filter'], t['min_filter'], t['mip_filter'], t['aniso_filter'], t['border_size']),
       (1, 0x688, -2, 1, 1, 2, 5, 1), 'dword 3 (swizzle at bit 1, exp_adjust signed 6-bit)')
    eq((t['vol_mag_filter'], t['vol_min_filter'], t['mip_min_level'], t['mip_max_level'], t['mag_aniso_walk'], t['lod_bias'],
        t['grad_exp_adjust_h'], t['grad_exp_adjust_v']), (1, 1, 2, 3, 1, -16, -16, 15), 'dword 4 (mip bounds here, LOD bias signed 10-bit)')
    eq((t['border_color'], t['force_bc_w_to_max'], t['tri_clamp'], t['aniso_bias'], t['dimension'], t['dimension_name'],
        t['packed_mips'], t['mip_address'], t['mip_address_bytes']), (2, 1, 3, -8, 1, 'k2DOrStacked', 1, 0x1FA00, 0x1FA00000), 'dword 5')
    eq(t['size'], dict(width=640, height=360, depth=6), '2D-or-stacked size = 2D view, depth = stack_depth + 1')
    eq(X.tfetch_dims(X.decode_tfetch(tfc(dim=0, w=100000))), (100000, 1, 1), '1D: 24-bit width')
    eq(X.tfetch_dims(X.decode_tfetch(tfc(dim=2, w=64, h=32, d=16))), (64, 32, 16), '3D view')
    eq(X.tfetch_dims(X.decode_tfetch(tfc(dim=3, w=128, h=128, d=6))), (128, 128, 6), 'cube: 2D view, depth 6')
    eq(X.tfetch_dims(X.decode_tfetch(tfc(dim=3, w=128, h=128, d=1))), (128, 128, 6), 'cube: depth is 6 whatever stack_depth says')
    eq(X.tfetch_dims(X.decode_tfetch(tfc(dim=1, w=128, h=128, d=1))), (128, 128, 1), '2D: depth = stack_depth + 1')
    nm = X.tfetch_names(t)
    eq((nm['endian'], nm['signs'][0], nm['clamps'][0], nm['mag'], nm['mip'], nm['aniso'], nm['border_color'], nm['swizzle_components']),
       ('k8in32', 'kSigned', 'kMirrorClampToBorder', 'kLinear', 'kBaseMap', 'kMax_16_1', 'k_ACBYCR_Black', [0, 1, 2, 3]), 'names')
    eq(t['raw'], raw, 'raw kept')
    eq(len(X.tfetch_field_vector(t)), 48, '48 struct fields')


@test('decode_tfetch_instruction: every TextureFetchInstruction field by hand (const index at bit 20, dimension at dword-2 bit 14, predicate at dword-1 bit 31, signed 7-bit LOD bias and 5-bit offsets), opcode names with dimension suffix, and the binding rule per opcode')
def _():
    d0, d1, d2 = tfi(opcode=17, const=13, dim=3, dst=9, dst_swiz=0x6C4, src=5, pred=1, cond=1, lod_bias=-3,
                     extra0=(1 << 11) | (1 << 18) | (1 << 19) | (1 << 25) | (0x2D << 26),
                     extra1=(1 << 12) | (2 << 14) | (3 << 16) | (5 << 18) | (7 << 21) | (1 << 24) | (2 << 26) | (1 << 28) | (1 << 29) | (1 << 30))
    d2 |= 1 | (1 << 1) | (0x1F << 9) | ((-8 & 0x1F) << 16) | (7 << 21) | ((-1 & 0x1F) << 26)
    i = X.decode_tfetch_instruction(d0, d1, d2)
    eq((i['opcode'], i['opcode_name'], i['opcode_known'], i['src_reg'], i['src_reg_am'], i['dst_reg'], i['dst_reg_am'],
        i['fetch_valid_only'], i['fetch_constant'], i['tx_coord_denorm'], i['src_swizzle']),
       (17, 'getCompTexLODCube', True, 5, 1, 9, 1, 1, 13, 1, 0x2D), 'dword 0')
    eq((i['dst_swizzle'], i['mag_filter'], i['min_filter'], i['mip_filter'], i['aniso_filter'], i['arbitrary_filter'],
        i['vol_mag_filter'], i['vol_min_filter'], i['use_computed_lod'], i['use_register_lod'], i['unk'], i['is_predicated']),
       (0x6C4, 1, 2, 3, 5, 7, 1, 2, 1, 1, 1, 1), 'dword 1')
    eq((i['use_register_gradients'], i['sample_location'], i['lod_bias'], i['unused'], i['dimension'], i['dimension_name'],
        i['offset_x'], i['offset_y'], i['offset_z'], i['predicate_condition']), (1, 1, -3, 0x1F, 3, 'kCube', -8, 7, -1, 1), 'dword 2')
    eq((i['writes_result'], i['is_binding']), (True, True), 'result written (swizzle 6C4 writes), binds')
    names = {op: X.decode_tfetch_instruction(*tfi(opcode=op, dim=2))['opcode_name'] for op in range(32)}
    eq((names[0], names[1], names[16], names[17], names[18], names[19], names[24], names[25], names[26], names[2], names[27]),
       ('vfetch', 'tfetch3D', 'getBCF3D', 'getCompTexLOD3D', 'getGradients', 'getWeights3D', 'setTexLOD', 'setGradientH',
        'setGradientV', 'unknown', 'unknown'), 'names (ParseTextureFetchInstruction L1058-L1085)')
    binds = {op: X.decode_tfetch_instruction(*tfi(opcode=op))['is_binding'] for op in range(32)}
    eq(sorted(op for op, b in binds.items() if b), [1, 16, 17, 18, 19], 'binding opcodes; setTexLOD/setGradientH/V and unknown never bind (L446-L452, L1090)')
    eq(X.decode_tfetch_instruction(*tfi(opcode=1, dst_swiz=0xFFF))['writes_result'], False, 'all-keep swizzle writes nothing')


@test('cf_exec_walk: the bound shrinks to the lowest exec address WHILE scanning (a later exec pointing into the CF block cuts it), every exec family names slots, no other CF opcode does, count 0 is an exec with no slots')
def _():
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(1, 2, 1, 0b01), cf(2, 1, 1, 0b01)) + cf_pair(cf(2, 0, 1, 0b01)) + tfi(const=6)))
    eq((w['pairs'], w['bound'], w['exec_count'], w['saw_exec'], w['malformed']), (3, 1, 2, True, False), 'bound 1: only pair 0 is CF')
    eq(w['slots'], [(0, 1, 2, True, False), (1, 2, 1, True, False)], 'slots from pair 0 only')
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(2, 0, 1, 0b01)) + tfi(const=6)))
    eq((w['bound'], w['saw_exec'], w['slots']), (0, False, []), 'exec at address 0: bound 0, nothing walked')
    for op in (1, 2, 3, 4, 5, 6, 13, 14):
        w = X.cf_exec_walk(be_bytes(cf_pair(cf(op, 1, 2, 0b0001)) + tfi(const=3) + alu3()))
        eq((w['exec_count'], w['slots']), (1, [(0, op, 1, True, False), (0, op, 2, False, False)]), 'exec family %d' % op)
    for op in (0, 7, 8, 9, 10, 11, 12, 15):
        w = X.cf_exec_walk(be_bytes(cf_pair(cf(op, 1, 2, 0b0101), cf(2, 1, 1, 0b01)) + tfi(const=3)))
        eq((w['exec_count'], w['saw_exec'], w['slots']), (1, True, [(1, 2, 1, True, False)]), 'non-exec CF %d names nothing; its exec partner does' % op)
        w = X.cf_exec_walk(be_bytes(cf_pair(cf(op, 1, 2, 0b0101)) + tfi(const=3) + alu3()))
        eq((w['exec_count'], w['saw_exec'], w['bound'], w['slots']), (0, False, 3, []),
           'alone, non-exec CF %d: no exec, whole program scanned as CF, nothing named' % op)
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(2, 1, 4, 0xE4)) + alu3() + tfi() + alu3() + tfi()))
    eq(w['slots'], [(0, 2, 1, False, False), (0, 2, 2, True, False), (0, 2, 3, False, True), (0, 2, 4, True, True)],
       'sequence bits: bit 0 fetch, bit 1 serialize, two per instruction')
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(2, 1, 0, 0)) + tfi()))
    eq((w['exec_count'], w['saw_exec'], w['slots']), (1, True, []), 'count 0')
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(2, 5, 1, 1))))
    eq((w['malformed'], w['malformed_slot'], w['slots']), (True, 5, []), 'slot beyond the ucode')
    w = X.cf_exec_walk(be_bytes(cf_pair(cf(2, 1, 2, 0b0101)) + tfi(const=1)))
    eq((w['malformed'], w['malformed_slot'], w['slots']), (True, 2, [(0, 2, 1, True, False)]), 'second slot beyond: first kept, walk stops')
    w = X.cf_exec_walk(b'')
    eq((w['dwords'], w['pairs'], w['bound'], w['slots']), (0, 0, 0, []), 'empty')
    eq(X.cf_exec_walk(be_bytes([1, 2]))['pairs'], 0, 'two dwords: no CF pair')


@test('analyze_texture_fetches: binding / non-binding / vertex / duplicate slots across two execs; the mask is deduplicated only at the end and every instruction keeps cf index, slot, opcode and constant')
def _():
    prog = (cf_pair(cf(1, 1, 4, 0b01010101), cf(2, 5, 3, 0b010101)) + tfi(const=2) + tfi(const=2, dim=2) + tfi(const=9)
            + tfi(opcode=24, const=2) + vfetch(const=0) + tfi(const=31) + tfi(opcode=25, const=30) + tfi(opcode=26, const=29))
    a = X.analyze_texture_fetches(be_bytes(prog))
    eq((a['status'], a['reason'], a['analysis_version']), ('complete', None, 'xtr_draws-static-texture-bindings/1'), 'complete')
    eq((a['ucode_dwords'], a['cf_pair_bound'], a['exec_instructions'], a['instruction_slots'], a['fetch_slots'], a['vertex_fetch_slots']),
       (27, 1, 2, 7, 7, 1), 'walk counts (slot 8 setGradientV is never named)')
    eq([(i['cf_index'], i['cf_opcode'], i['slot'], i['opcode_name'], i['fetch_constant'], i['is_binding']) for i in a['instructions']],
       [(0, 1, 1, 'tfetch2D', 2, True), (0, 1, 2, 'tfetch3D', 2, True), (0, 1, 3, 'tfetch2D', 9, True), (0, 1, 4, 'setTexLOD', 2, False),
        (1, 2, 6, 'tfetch2D', 31, True), (1, 2, 7, 'setGradientH', 30, False)], 'instructions in program order (duplicates kept)')
    eq((a['binding_slots'], a['binding_mask'], a['binding_instructions'], a['non_binding_instructions'], a['unknown_opcode_instructions']),
       ([2, 9, 31], (1 << 2) | (1 << 9) | (1 << 31), 4, 2, 0), 'mask deduplicated; setTexLOD/setGradientH bind nothing')
    a = X.analyze_texture_fetches(be_bytes(ps_prog(tfi(opcode=18, const=5))))
    eq((a['status'], a['binding_slots'], a['instructions'][0]['opcode_name']), ('complete', [5], 'getGradients'),
       'getGradients binds in the ANALYSIS stage (GatherTextureFetchInformation only excludes 24/25/26)')
    a = X.analyze_texture_fetches(be_bytes(ps_prog(tfi(opcode=24, const=1), tfi(opcode=25, const=2), tfi(opcode=26, const=3))))
    eq((a['status'], a['binding_slots'], a['binding_instructions'], a['non_binding_instructions']), ('complete', [], 0, 3), 'setters only')


@test('analyze_texture_fetches fails closed: unknown fetch opcode, exec beyond the ucode, no exec, empty ucode are named incomplete states with the partial instruction list kept and nothing invented')
def _():
    a = X.analyze_texture_fetches(be_bytes(ps_prog(tfi(const=1), tfi(opcode=27, const=3))))
    eq((a['status'], a['binding_slots'], a['unknown_opcode_instructions'], a['instructions'][1]['opcode_name'], a['instructions'][1]['is_binding']),
       ('incomplete', [1], 1, 'unknown', False), 'unknown opcode')
    ck('unknown fetch opcode 27@slot2' in a['reason'] and 'assert_unhandled_case' in a['reason'], 'reason %r' % a['reason'])
    a = X.analyze_texture_fetches(be_bytes(cf_pair(cf(2, 1, 2, 0b0101)) + tfi(const=1)))
    eq((a['status'], a['binding_slots'], len(a['instructions'])), ('incomplete', [1], 1), 'malformed: first slot kept')
    ck('slot 2 beyond the ucode (6 dwords)' in a['reason'], 'reason %r' % a['reason'])
    a = X.analyze_texture_fetches(be_bytes(cf_pair(cf(0, 0), cf(12, 0)) + tfi(const=1)))
    eq((a['status'], a['binding_slots']), ('incomplete', []), 'no exec')
    ck(a['reason'].startswith('no exec-class control-flow instruction'), 'reason %r' % a['reason'])
    a = X.analyze_texture_fetches(b'')
    eq((a['status'], a['reason'], a['ucode_dwords']), ('incomplete', 'empty ucode', 0), 'empty')
    a = X.analyze_texture_fetches(be_bytes(cf_pair(cf(2, 1, 0, 0)) + tfi(const=1)))
    eq((a['status'], a['binding_slots'], a['exec_instructions']), ('complete', [], 1), 'count 0 exec: complete with no binding')


@test('T3A wiring: a normal draw row - VS with no texture, PS naming slots 1 and 4 plus a setTexLOD; union, provenance, raw + decoded constants of the referenced slots, the unused type-2 slot 6, and the conserving census with T3A1 denominators')
def _():
    p = binding_trace('t3a_basic', vs_simple(), PS_A, CONSTS_A)
    s, eng = summary5(p)
    rows = s['static_texture_bindings']
    eq(len(rows), 1, 'one normal draw, one row')
    r = rows[0]
    eq(list(r), ['draw_ord', 'frame', 'mode', 'vs', 'ps', 'analysis_status', 'stages', 'union_slots', 'overlap_slots', 'bindings',
                 'type2_slots', 'unused_type2_slots', 'referenced_non_texture_slots'], 'row keys')
    eq((r['draw_ord'], r['frame'], r['mode'], r['vs'], r['ps'], r['analysis_status']),
       (1, 0, 4, eng.draws[0]['vs'], eng.draws[0]['ps'], 'complete'), 'identity')
    vs, ps = r['stages']['vs'], r['stages']['ps']
    eq(list(ps), ['status', 'reason', 'ucode_hash', 'binding_slots', 'binding_mask', 'instructions', 'binding_instructions',
                  'non_binding_instructions', 'unknown_opcode_instructions'], 'stage keys')
    eq((vs['status'], vs['reason'], vs['ucode_hash'], vs['binding_slots'], vs['instructions']), ('complete', None, r['vs'], [], []), 'VS: no texture')
    eq((ps['status'], ps['ucode_hash'], ps['binding_slots'], ps['binding_mask'], ps['binding_instructions'], ps['non_binding_instructions']),
       ('complete', r['ps'], [1, 4], 0x12, 2, 1), 'PS')
    eq([(i['cf_index'], i['cf_opcode'], i['slot'], i['opcode'], i['opcode_name'], i['fetch_constant'], i['dimension'], i['dimension_name'],
         i['is_binding'], i['writes_result'], i['is_predicated']) for i in ps['instructions']],
       [(0, 2, 1, 1, 'tfetch2D', 1, 1, 'k2D', True, True, 0), (0, 2, 2, 1, 'tfetch3D', 4, 2, 'k3DOrStacked', True, True, 0),
        (0, 2, 3, 24, 'setTexLOD', 1, 1, 'k2D', False, True, 0)], 'PS instructions with provenance')
    eq((r['union_slots'], r['overlap_slots'], r['type2_slots'], r['unused_type2_slots'], r['referenced_non_texture_slots']),
       ([1, 4], [], [1, 4, 6], [6], []), 'slot sets')
    eq([list(b) for b in r['bindings']], [['slot', 'stages', 'instructions_per_stage', 'fetch_constant_type', 'fetch_constant_type_name',
                                            'is_texture_constant', 'raw', 'decoded']] * 2, 'binding keys')
    b1, b4 = r['bindings']
    eq((b1['slot'], b1['stages'], b1['instructions_per_stage'], b1['fetch_constant_type'], b1['fetch_constant_type_name'], b1['is_texture_constant'], b1['raw']),
       (1, ['ps'], dict(vs=0, ps=1), 2, 'kTexture', True, TFC_1), 'slot 1 binding')
    d = b1['decoded']
    eq((d['type'], d['format'], d['format_name'], d['dimension'], d['dimension_name'], d['size'], d['base_address_bytes'], d['mip_address_bytes'],
        d['packed_mips'], d['mip_max_level'], d['pitch_pixels'], d['swizzle'], d['tiled'], d['endianness']),
       (2, 20, 'k_DXT4_5', 1, 'k2DOrStacked', dict(width=512, height=512, depth=1), 0x11060000, 0x110A0000, 1, 9, 512, 0x688, 1, 2), 'slot 1 decode')
    eq(d, json.loads(json.dumps(X.decode_tfetch(TFC_1))), 'decoded == decode_tfetch(raw)')
    eq((b4['slot'], b4['raw'], b4['decoded']['dimension_name'], b4['decoded']['size']), (4, TFC_4, 'k3D', dict(width=64, height=64, depth=8)), 'slot 4 binding')
    c = s['static_texture_binding_census']
    eq((c['analysis_version'], c['normal_draws_total'], c['draw_packets_total'], c['excluded']),
       ('xtr_draws-static-texture-bindings/1', 1, 1, dict(resolves=0, noop_draws=0, unknown_mode_draws=0)), 'denominators')
    eq(c['shader_analysis'], dict(vs=dict(complete=1, incomplete=0, missing=0), ps=dict(complete=1, incomplete=0, missing=0),
                                  draws_both_complete=1, draws_not_complete=0), 'shader analysis')
    eq(c['referenced_instructions']['ps'], dict(binding=2, non_binding=1, unknown_opcode=0, by_opcode={'setTexLOD': 1, 'tfetch2D': 1, 'tfetch3D': 1}), 'PS instructions')
    eq(c['referenced_instructions']['vs'], dict(binding=0, non_binding=0, unknown_opcode=0, by_opcode={}), 'VS instructions')
    eq((c['referenced_slots']['vs'], c['referenced_slots']['ps'], c['referenced_slots']['union'], c['referenced_slots']['overlap']), (0, 2, 2, 0), 'slots')
    eq(c['referenced_slots_by_fetch_constant_type'], dict(kInvalidTexture=0, kInvalidVertex=0, kTexture=2, kVertex=0), 'by type')
    eq((c['type2_slots_total'], c['unused_type2_slots_total'], c['draws_with_unused_type2'], c['draws_with_referenced_non_texture_slot']), (3, 1, 1, 0), 'type-2 census')
    eq((c['draws_with_static_binding'], c['draws_with_no_static_binding'], c['draws_analysis_not_complete'], c['conservation_holds']), (1, 0, 0, True), 'conserved')
    eq(c['t3a1'], dict(claim='every type-2 fetch constant at a normal draw is a texture consumer of that draw', complete_draws=1,
                       type2_set_equals_union=0, type2_has_unused=1, union_has_non_type2=0), 'T3A1: slot 6 is type 2 but unreferenced')
    eq(c['distinct_shaders'], dict(vs=dict(complete=1, incomplete=0), ps=dict(complete=1, incomplete=0)), 'distinct')
    ck('post-translation' in c['meaning'] and 'not' in c['meaning'], 'the census says what it is not')
    js = json.dumps(r).lower()
    for banned in ('extent', 'producer', 'owner', 'contents', 'consumer_range', 'join_usable'):
        ck(banned not in js, 'no consumer/producer claim in the row: %s' % banned)


@test('T3A wiring: stage overlap and a VS-only slot - the VS samples slot 4 (also in the PS) and slot 7 (VS only): union [1, 4, 7], overlap [4], stages per binding with per-stage counts, census vs/ps/union/overlap = 2/2/3/1')
def _():
    vs = cf_pair(cf(2, 1, 3, 0b010101)) + vfetch(fmt=57) + tfi(const=4, dim=1) + tfi(const=7, dim=0)
    s, eng = summary5(binding_trace('t3a_overlap', vs, PS_A, CONSTS_A + [(7, tfc(fmt=2, dim=0, base=0x1FB00000, w=64))]))
    r = s['static_texture_bindings'][0]
    eq((r['stages']['vs']['binding_slots'], r['union_slots'], r['overlap_slots']), ([4, 7], [1, 4, 7], [4]), 'union of both stages, overlap only the shared slot')
    eq([(b['slot'], b['stages'], b['instructions_per_stage']) for b in r['bindings']],
       [(1, ['ps'], dict(vs=0, ps=1)), (4, ['vs', 'ps'], dict(vs=1, ps=1)), (7, ['vs'], dict(vs=1, ps=0))], 'provenance per binding')
    eq(r['bindings'][2]['decoded']['size'], dict(width=64, height=1, depth=1), 'VS-only slot decoded')
    c = s['static_texture_binding_census']
    eq((c['referenced_slots']['vs'], c['referenced_slots']['ps'], c['referenced_slots']['union'], c['referenced_slots']['overlap']), (2, 2, 3, 1), 'census slots')
    eq(c['referenced_instructions']['vs'], dict(binding=2, non_binding=0, unknown_opcode=0, by_opcode={'tfetch1D': 1, 'tfetch2D': 1}), 'VS instructions counted')
    eq(s['vfetch_census']['attrs'], 1, 'the shared walk still feeds the vertex census')


@test('T3A wiring: a referenced slot whose constant is not type 2 (kVertex / kInvalidTexture) is reported as a non-texture reference, never dropped and never promoted; T3A1 sees the union not contained in the type-2 set')
def _():
    for type_, name in ((3, 'kVertex'), (0, 'kInvalidTexture')):
        consts = [(1, TFC_1), (4, tfc(type_=type_, fmt=6, dim=2, w=64, h=64, d=8)), (6, TFC_6)]
        s, eng = summary5(binding_trace('t3a_nontex_%d' % type_, vs_simple(), PS_A, consts))
        r = s['static_texture_bindings'][0]
        b4 = r['bindings'][1]
        eq((r['union_slots'], r['type2_slots'], r['unused_type2_slots'], r['referenced_non_texture_slots']), ([1, 4], [1, 6], [6], [4]), '%s: slot sets' % name)
        eq((b4['fetch_constant_type'], b4['fetch_constant_type_name'], b4['is_texture_constant'], b4['decoded']['type_name']), (type_, name, False, name), '%s: binding' % name)
        c = s['static_texture_binding_census']
        eq((c['draws_with_referenced_non_texture_slot'], c['referenced_slots_by_fetch_constant_type'][name], c['referenced_slots_by_fetch_constant_type']['kTexture'],
            c['t3a1']['union_has_non_type2'], c['t3a1']['type2_set_equals_union']), (1, 1, 1, 1, 0), '%s: census' % name)


@test('T3A fail closed at the draw: no pixel shader loaded -> stage missing (named), row incomplete, NO fallback to the type-2 constants, census not_complete with conservation; no vertex shader -> the PS references are kept but the draw is still not complete')
def _():
    s, eng = summary5(binding_trace('t3a_no_ps', vs_simple(), None, [(1, TFC_1), (6, TFC_6)]))
    r = s['static_texture_bindings'][0]
    ps = r['stages']['ps']
    eq((ps['status'], ps['reason'], ps['ucode_hash'], ps['binding_slots'], ps['binding_mask'], ps['instructions']),
       ('missing', 'no pixel shader loaded before this draw', None, [], 0, []), 'missing PS')
    eq((r['analysis_status'], r['union_slots'], r['bindings'], r['type2_slots'], r['unused_type2_slots']), ('incomplete', [], [], [1, 6], [1, 6]), 'nothing invented')
    c = s['static_texture_binding_census']
    eq((c['shader_analysis']['ps'], c['shader_analysis']['draws_both_complete'], c['shader_analysis']['draws_not_complete']),
       (dict(complete=0, incomplete=0, missing=1), 0, 1), 'census stages')
    eq((c['draws_with_static_binding'], c['draws_with_no_static_binding'], c['draws_analysis_not_complete'], c['conservation_holds'],
        c['type2_slots_total'], c['unused_type2_slots_total'], c['draws_with_unused_type2'], c['t3a1']['complete_draws']),
       (0, 0, 1, True, 0, 0, 0, 0), 'complete-only denominators: the incomplete draw contributes no type-2 counts')
    s, eng = summary5(binding_trace('t3a_no_vs', None, PS_A, CONSTS_A))
    r = s['static_texture_bindings'][0]
    eq((r['stages']['vs']['status'], r['stages']['vs']['reason'], r['analysis_status'], r['union_slots'], len(r['bindings'])),
       ('missing', 'no vertex shader loaded before this draw', 'incomplete', [1, 4], 2), 'missing VS: PS references kept, draw not complete')
    c = s['static_texture_binding_census']
    eq((c['draws_with_static_binding'], c['draws_analysis_not_complete'], c['referenced_slots']['union'], c['conservation_holds']), (0, 1, 0, True),
       'an incomplete draw never counts as having a binding')


@test('T3A fail closed at the shader: malformed exec, no exec and an unknown fetch opcode make the PS stage incomplete with the reason on the row; partly unmapped IM_LOAD ucode is incomplete and unhashed; --no-shader-hash still analyses the bytes')
def _():
    s, eng = summary5(binding_trace('t3a_malformed', vs_simple(), cf_pair(cf(2, 5, 1, 1)), CONSTS_A))
    r = s['static_texture_bindings'][0]
    eq((r['stages']['ps']['status'], r['analysis_status']), ('incomplete', 'incomplete'), 'malformed')
    ck('beyond the ucode' in r['stages']['ps']['reason'], 'reason %r' % r['stages']['ps']['reason'])
    s, eng = summary5(binding_trace('t3a_noexec', vs_simple(), cf_pair(cf(0, 0)) + [0, 0, 0], CONSTS_A))
    ck(s['static_texture_bindings'][0]['stages']['ps']['reason'].startswith('no exec-class'), 'no exec')
    s, eng = summary5(binding_trace('t3a_unknown', vs_simple(), ps_prog(tfi(const=1), tfi(opcode=27, const=3)), CONSTS_A))
    r = s['static_texture_bindings'][0]
    eq((r['stages']['ps']['status'], r['stages']['ps']['binding_slots'], r['stages']['ps']['unknown_opcode_instructions'], r['analysis_status']),
       ('incomplete', [1], 1, 'incomplete'), 'unknown opcode')
    c = s['static_texture_binding_census']
    eq((c['referenced_instructions']['ps']['unknown_opcode'], c['referenced_instructions']['ps']['by_opcode'], c['shader_analysis']['ps']['incomplete']),
       (1, {'tfetch2D': 1, 'unknown': 1}, 1), 'census sees the unknown opcode')
    ps = PS_A
    tr = base_trace().packet(im_load_imm(0, vs_simple())).mem_read(0x1F800000, be_bytes(ps)[:8]) \
        .packet(im_load(1, 0x1F800000, len(ps)))
    for slot, dw in CONSTS_A:
        tr.packet(set_const(1, slot * 6, *dw))
    p = tr.packet(draw2()).save('t3a_unmapped')
    s, eng = summary5(p)
    r = s['static_texture_bindings'][0]
    eq((r['ps'], r['stages']['ps']['ucode_hash'], r['stages']['ps']['status']), (None, None, 'incomplete'), 'unmapped ucode: not hashed, incomplete')
    ck(r['stages']['ps']['reason'].startswith('ucode partly unmapped'), 'reason %r' % r['stages']['ps']['reason'])
    eq(sorted(s['static_texture_shader_analyses']), ['%016X' % eng.draws[0]['vs']], 'only the hashed VS is in the analyses table')
    s, eng = summary5(binding_trace('t3a_nohash', vs_simple(), PS_A, CONSTS_A), hash_shaders=False)
    r = s['static_texture_bindings'][0]
    eq((r['vs'], r['ps'], r['stages']['ps']['ucode_hash'], r['stages']['ps']['status'], r['union_slots'], s['static_texture_shader_analyses']),
       (None, None, None, 'complete', [1, 4], {}), 'unhashed: analysed from the bytes, no table entry')


@test('T3A: kCopy resolves are not normal draws (no row, counted as excluded); the same shaders over three draws give three identical rows and one table entry per hash equal to the analysis; a PS with no texture fetch is a complete draw with no static binding (T3A1 equality only when no type-2 constant exists)')
def _():
    s, eng = summary5(binding_trace('t3a_resolve', vs_simple(), PS_A, CONSTS_A, mode=6))
    c = s['static_texture_binding_census']
    eq((s['static_texture_bindings'], c['normal_draws_total'], c['draw_packets_total'], c['excluded'], c['conservation_holds']),
       ([], 0, 1, dict(resolves=1, noop_draws=0, unknown_mode_draws=0), True), 'resolve excluded')
    s, eng = summary5(binding_trace('t3a_reuse', vs_simple(), PS_A, CONSTS_A, draws=3))
    rows = s['static_texture_bindings']
    eq([r['draw_ord'] for r in rows], [1, 2, 3], 'three rows')
    eq(len({json.dumps(dict(r, draw_ord=0), sort_keys=True) for r in rows}), 1, 'identical apart from the ordinal')
    c = s['static_texture_binding_census']
    eq((c['normal_draws_total'], c['shader_analysis']['draws_both_complete'], c['referenced_slots']['ps'], c['type2_slots_total'],
        c['unused_type2_slots_total'], c['t3a1']['complete_draws'], c['distinct_shaders']['ps']['complete']), (3, 3, 6, 9, 3, 3, 1), 'x3 census, 1 distinct PS')
    tab = s['static_texture_shader_analyses']
    eq(sorted(tab), sorted('%016X' % h for h in (eng.draws[0]['vs'], eng.draws[0]['ps'])), 'table keyed by 16-hex hash')
    eq(tab['%016X' % eng.draws[0]['ps']], json.loads(json.dumps(X.analyze_texture_fetches(be_bytes(PS_A)))), 'table entry == analysis')
    eq(tab['%016X' % eng.draws[0]['ps']]['instructions'][0]['lod_bias'], 0, 'the table carries the full instruction decode')
    s, eng = summary5(binding_trace('t3a_nobind', vs_simple(), ps_prog(), [(1, TFC_1)]))
    r = s['static_texture_bindings'][0]
    c = s['static_texture_binding_census']
    eq((r['analysis_status'], r['union_slots'], r['unused_type2_slots'], c['draws_with_static_binding'], c['draws_with_no_static_binding'],
        c['t3a1']['type2_set_equals_union'], c['t3a1']['type2_has_unused']), ('complete', [], [1], 0, 1, 0, 1), 'no binding, unused type-2')
    s, eng = summary5(binding_trace('t3a_nobind_noconst', vs_simple(), ps_prog(), []))
    c = s['static_texture_binding_census']
    eq((c['draws_with_no_static_binding'], c['t3a1']['type2_set_equals_union'], c['t3a1']['type2_has_unused']), (1, 1, 0), 'both sets empty: T3A1 equality')


@test('T3A JSON/CLI wiring (/7): schema /7 and tool version; the /6 tables in the file equal the summary and expand to the /5 projection; every /4 key preserved; --textures prints the static bindings with provenance, the decoded constant and the unreferenced type-2 list; a resolve draw says so; the summary prints the bindings line')
def _():
    tr = base_trace().packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, PS_A))
    for slot, dw in CONSTS_A:
        tr.packet(set_const(1, slot * 6, *dw))
    tr.packet(draw2()).regs(0x2208, [6]).packet(vf0(VF0_ADDR)).packet_start(draw2()).mem_read(VF0_ADDR, be_bytes(VERTS_A)).packet_end()
    p = tr.save('t3a_cli')
    s, eng = summary(p)
    out = os.path.join(TMP, 't3a.json')
    rc, so, se = run_main([p, '--json', out, '--draw', '1', '--textures'])
    eq(rc, 0, 'exit; stderr=%r' % se)
    j = json.load(open(out, encoding='utf-8'))
    eq((j['schema'], j['tool_version'], X.SCHEMA, X.TOOL_VERSION), ('xtr_draws-summary/7', X.TOOL_VERSION, 'xtr_draws-summary/7', '2.6.0'), 'schema /7')
    for k in ('static_texture_shaders', 'texture_fetch_constants', 'static_texture_bindings', 'static_texture_binding_census',
              'texture_backing_manifest', 'requested_binding_census'):
        eq(j[k], json.loads(json.dumps(s[k])), 'json key %s' % k)
    v5 = X.expand_static_bindings_v5(j)
    eq(sorted(v5), ['static_texture_binding_census', 'static_texture_bindings', 'static_texture_shader_analyses'], 'the /5 projection keys')
    eq((len(v5['static_texture_bindings']), len(v5['static_texture_shader_analyses']), v5['static_texture_bindings'][0]['union_slots']),
       (1, 2, [1, 4]), 'the /5 projection from the file')
    for k in ('resolve_details', 'resolve_inputs', 'derivation_manifest', 'guest_destination_ranges', 'vfetch_census', 'distinct_shaders'):
        ck(k in j, '/4 key %s preserved' % k)
    eq(len(j['resolve_details']), 1, 'the resolve row is still a resolve row')
    eq((j['resolve_details'][0]['guest_destination_range']['status'], j['draws'], j['resolves'], len(j['static_texture_bindings'])),
       ('empty_rectangle', 1, 1, 1), 'resolve derived as before; one normal draw row')
    ck('static texture bindings (complete; vs=complete ps=complete) union=[1, 4] overlap=[] unusedType2=[6] nonTexture=[]' in so, 'header line: %r' % so[-2500:])
    ck('    ps cf0 slot1 tfetch2D fc=1 dim=k2D binding=1 writes=1 pred=0' in so and '    ps cf0 slot3 setTexLOD fc=1 dim=k2D binding=0 writes=1 pred=0' in so,
       'instruction lines')
    ck('    t1  [ps] type=kTexture k_DXT4_5(20) k2DOrStacked 512x512x1 base=11060000 mip=110A0000 pitch=512 tiled=1 endian=k8in32 stacked=0 packedMips=1 swz=688 mips=0..9' in so,
       'decoded binding line: %r' % so[-2500:])
    ck('type-2 fetch constants NOT statically referenced by either shader (no consumer evidence):' in so and '    t6  k_8(2) k1D 256x1x1 base=1FA00000' in so,
       'unreferenced type-2 list')
    ck('bindings: normalDraws=1 bothComplete=1 withBinding=1 noBinding=0 notComplete=0 conserved=1 | slots vs=0 ps=2 union=2 overlap=0 type2=3 unusedType2=1 nonTextureRefs=0 | T3A1 type2==union 0/1 draws' in so,
       'summary line: %r' % so[:2500])
    rc, so, se = run_main([p, '--draw', '2', '--textures'])
    ck("static texture bindings: none - draw kind 'resolve' is not a normal draw in this census" in so, 'resolve draw view')


# ── TOOL-3B: requested bindings and texture backing (schema /6) ─────────────

TITLE_TFC = [0x84020002, 0x11060054, 0x003FE1FF, 0x00A80D10, 0x00000243, 0x110A0A18]   # a title 512x512 DXT4_5, packed, mips 0..9
FMV_TFC = [0x06000002, 0x0E2C0002, 0x002CE27F, 0x00A81400, 0x00000003, 0x00000200]     # an FMV 640x360 k_8 linear plane
F = {name: i for i, (name, _t, _b) in enumerate(X.FORMAT_TABLE)}


def fkey(raw):
    return ''.join('%08X' % x for x in raw)


def pm_entry(stage, prog, post_mask=None, valid=1, xenia_mask=None):
    """One recorded actual-translator row for a synthetic program (the shape mask_oracle.py writes)."""
    uc = be_bytes(prog)
    a = X.analyze_texture_fetches(uc)
    m = a['binding_mask']
    return dict(stage=stage, file='%s_test.bin' % stage, ucode_sha256=hashlib.sha256(uc).hexdigest(), ucode_bytes=len(uc),
                xenia_analysis_mask='%08X' % (m if xenia_mask is None else xenia_mask), translated=1, translation_valid=valid,
                translation_errors=0 if valid else 1, post_mask='%08X' % (m if post_mask is None else post_mask),
                post_bindings=[dict(fetch_constant=i['fetch_constant'], dimension=i['dimension'], is_signed=0)
                               for i in a['instructions'] if i['is_binding']])


def pm_table(name, entries, pin=None):
    path = os.path.join(TMP, name + '_masks.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(dict(identity=dict(xenia_pin=pin or X.XENIA_PIN, oracle_source_sha256='0' * 64, oracle_exe_sha256='1' * 64,
                                     translator_options='test table'), summary=dict(shaders=len(entries)), shaders=entries), f)
    return path


VS_OVERLAP = cf_pair(cf(2, 1, 3, 0b010101)) + vfetch(fmt=57) + tfi(const=4, dim=1) + tfi(const=7, dim=0)
TFC_7 = tfc(fmt=2, dim=0, base=0x1FB00000, w=64)
TFC_NODATA = tfc(fmt=6, dim=1, base=0, mipbase=0, w=64, h=64, pitch=2)


LAYOUT_ORACLE_VECTORS = [
    ('2d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 2), (1, 0, 4294967295, 0, 1, 0),
     (32, 32, 4096, 1, 1, 1, 1, 1), {}, {}),
    ('2d', [0x00400002, 0x00000082, 0x00000000, 0x00A80D10, 0x00000340, 0x11100200],
     (0, 0, 0, 0, 0, 0, 0), None, None,
     None, {}, {}),
    ('3d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 2), (1, 0, 4294967295, 0, 1, 0),
     (32, 32, 4096, 1, 1, 1, 1, 1), {}, {}),
    ('cube', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 2, 2), (6, 0, 4294967295, 0, 20481, 0),
     (32, 32, 4096, 1, 1, 1, 1, 20481), {}, {}),
    ('1d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 2), (1, 0, 4294967295, 0, 1, 0),
     (32, 1, 4096, 1, 1, 1, 1, 1), {}, {}),
    ('1d', [0x40400002, 0x1F900082, 0x00002000, 0x00A80D10, 0x00000000, 0x00000000],
     (8192, 0, 0, 129280, 0, 0, 0), None, None,
     None, {}, {}),
    ('2d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 2, 2), (1, 0, 0, 0, 17, 0),
     (32, 32, 4096, 17, 1, 1, 17, 17), {}, {}),
    ('2d', [0x00400002, 0x00000082, 0x00000000, 0x00A80D10, 0x00000340, 0x11100A00],
     (0, 0, 0, 0, 0, 0, 0), None, None,
     None, {}, {}),
    ('3d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 2, 2), (1, 0, 0, 0, 17, 0),
     (32, 32, 4096, 17, 1, 1, 17, 17), {}, {}),
    ('cube', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 2, 2), (6, 0, 0, 0, 20497, 0),
     (32, 32, 4096, 17, 1, 1, 17, 20497), {}, {}),
    ('1d', [0x00400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000800],
     (0, 0, 0, 129280, 0, 0, 0), None, None,
     None, {}, {}),
    ('2d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 2, 2), (1, 0, 4294967295, 0, 2560, 0),
     (32, 32, 4096, 1, 1, 1, 2560, 2560), {}, {}),
    ('2d', [0x80400002, 0x00000082, 0x00000000, 0x00A80D10, 0x00000340, 0x11100200],
     (0, 0, 0, 0, 0, 0, 0), None, None,
     None, {}, {}),
    ('3d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 2, 2), (1, 0, 4294967295, 0, 4096, 0),
     (32, 32, 4096, 1, 1, 1, 4096, 4096), {}, {}),
    ('cube', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 2, 2), (6, 0, 4294967295, 0, 23040, 0),
     (32, 32, 4096, 1, 1, 1, 2560, 23040), {}, {}),
    ('1d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), None, None,
     None, {}, {}),
    ('2d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 2, 2), (1, 0, 0, 0, 2560, 0),
     (32, 32, 4096, 17, 1, 1, 2560, 2560), {}, {}),
    ('2d', [0x80400002, 0x00000082, 0x00000000, 0x00A80D10, 0x00000340, 0x11100A00],
     (0, 0, 0, 0, 0, 0, 0), None, None,
     None, {}, {}),
    ('3d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 2, 2), (1, 0, 0, 0, 4096, 0),
     (32, 32, 4096, 17, 1, 1, 4096, 4096), {}, {}),
    ('cube', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 2, 2), (6, 0, 0, 0, 23040, 0),
     (32, 32, 4096, 17, 1, 1, 2560, 23040), {}, {}),
    ('1d', [0x80400002, 0x1F900082, 0x00000000, 0x00A80D10, 0x00000000, 0x00000800],
     (0, 0, 0, 129280, 0, 0, 0), None, None,
     None, {}, {}),
    ('2d', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 6, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('3d', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 6, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 16384, 1, 1, 1, 4, 4), {}, {}),
    ('cube', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 6, 2), (6, 0, 4294967295, 0, 20484, 0),
     (128, 32, 4096, 1, 1, 1, 4, 20484), {}, {}),
    ('1d', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 6, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 1, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('2d', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 6, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 4096, 17, 1, 1, 68, 68), {}, {}),
    ('3d', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 6, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 16384, 17, 1, 1, 68, 68), {}, {}),
    ('cube', [0x00400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 6, 2), (6, 0, 0, 0, 20548, 0),
     (128, 32, 4096, 17, 1, 1, 68, 20548), {}, {}),
    ('2d', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 6, 2), (1, 0, 4294967295, 0, 4096, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 6, 2), (1, 0, 4294967295, 0, 16384, 0),
     (128, 32, 16384, 1, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 6, 2), (6, 0, 4294967295, 0, 24576, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 6, 2), (1, 0, 0, 0, 4096, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 6, 2), (1, 0, 0, 0, 16384, 0),
     (128, 32, 16384, 17, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F900086, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 6, 2), (6, 0, 0, 0, 24576, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 14, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('3d', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 14, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 16384, 1, 1, 1, 4, 4), {}, {}),
    ('cube', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 14, 2), (6, 0, 4294967295, 0, 20484, 0),
     (128, 32, 4096, 1, 1, 1, 4, 20484), {}, {}),
    ('1d', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 14, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 1, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('2d', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 14, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 4096, 17, 1, 1, 68, 68), {}, {}),
    ('3d', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 14, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 16384, 17, 1, 1, 68, 68), {}, {}),
    ('cube', [0x00400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 14, 2), (6, 0, 0, 0, 20548, 0),
     (128, 32, 4096, 17, 1, 1, 68, 20548), {}, {}),
    ('2d', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 14, 2), (1, 0, 4294967295, 0, 4096, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 14, 2), (1, 0, 4294967295, 0, 16384, 0),
     (128, 32, 16384, 1, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 14, 2), (6, 0, 4294967295, 0, 24576, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 14, 2), (1, 0, 0, 0, 4096, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 14, 2), (1, 0, 0, 0, 16384, 0),
     (128, 32, 16384, 17, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F90008E, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 14, 2), (6, 0, 0, 0, 24576, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 0, 18, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 32, 8192, 1, 1, 1, 8, 8), {}, {}),
    ('3d', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 0, 18, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 32, 32768, 1, 1, 1, 8, 8), {}, {}),
    ('cube', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 0, 18, 2), (6, 0, 4294967295, 0, 40968, 0),
     (256, 32, 8192, 1, 1, 1, 8, 40968), {}, {}),
    ('1d', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 4, 0, 0, 0, 18, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 1, 4096, 1, 1, 1, 8, 8), {}, {}),
    ('2d', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 1, 18, 2), (1, 0, 0, 0, 40, 0),
     (256, 32, 8192, 5, 1, 1, 40, 40), {}, {}),
    ('3d', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 1, 18, 2), (1, 0, 0, 0, 40, 0),
     (256, 32, 32768, 5, 1, 1, 40, 40), {}, {}),
    ('cube', [0x01000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 1, 18, 2), (6, 0, 0, 0, 41000, 0),
     (256, 32, 8192, 5, 1, 1, 40, 41000), {}, {}),
    ('2d', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 0, 18, 2), (1, 0, 4294967295, 0, 8192, 0),
     (256, 32, 8192, 1, 1, 1, 8192, 8192), {}, {}),
    ('3d', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 0, 18, 2), (1, 0, 4294967295, 0, 32768, 0),
     (256, 32, 32768, 1, 1, 1, 32768, 32768), {}, {}),
    ('cube', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 0, 18, 2), (6, 0, 4294967295, 0, 49152, 0),
     (256, 32, 8192, 1, 1, 1, 8192, 49152), {}, {}),
    ('2d', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 1, 18, 2), (1, 0, 0, 0, 8192, 0),
     (256, 32, 8192, 5, 1, 1, 8192, 8192), {}, {}),
    ('3d', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 1, 18, 2), (1, 0, 0, 0, 32768, 0),
     (256, 32, 32768, 5, 1, 1, 32768, 32768), {}, {}),
    ('cube', [0x81000002, 0x1F900092, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 1, 18, 2), (6, 0, 0, 0, 49152, 0),
     (256, 32, 8192, 5, 1, 1, 8192, 49152), {}, {}),
    ('2d', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 0, 20, 2), (1, 0, 4294967295, 0, 16, 0),
     (512, 32, 16384, 1, 1, 1, 16, 16), {}, {}),
    ('3d', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 0, 20, 2), (1, 0, 4294967295, 0, 16, 0),
     (512, 32, 65536, 1, 1, 1, 16, 16), {}, {}),
    ('cube', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 0, 20, 2), (6, 0, 4294967295, 0, 81936, 0),
     (512, 32, 16384, 1, 1, 1, 16, 81936), {}, {}),
    ('1d', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 4, 0, 0, 0, 20, 2), (1, 0, 4294967295, 0, 16, 0),
     (512, 1, 4096, 1, 1, 1, 16, 16), {}, {}),
    ('2d', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 1, 20, 2), (1, 0, 0, 0, 80, 0),
     (512, 32, 16384, 5, 1, 1, 80, 80), {}, {}),
    ('3d', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 1, 20, 2), (1, 0, 0, 0, 80, 0),
     (512, 32, 65536, 5, 1, 1, 80, 80), {}, {}),
    ('cube', [0x01000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 1, 20, 2), (6, 0, 0, 0, 82000, 0),
     (512, 32, 16384, 5, 1, 1, 80, 82000), {}, {}),
    ('2d', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 0, 20, 2), (1, 0, 4294967295, 0, 16384, 0),
     (512, 32, 16384, 1, 1, 1, 16384, 16384), {}, {}),
    ('3d', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 0, 20, 2), (1, 0, 4294967295, 0, 65536, 0),
     (512, 32, 65536, 1, 1, 1, 65536, 65536), {}, {}),
    ('cube', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 0, 20, 2), (6, 0, 4294967295, 0, 98304, 0),
     (512, 32, 16384, 1, 1, 1, 16384, 98304), {}, {}),
    ('2d', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 1, 20, 2), (1, 0, 0, 0, 16384, 0),
     (512, 32, 16384, 5, 1, 1, 16384, 16384), {}, {}),
    ('3d', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 1, 20, 2), (1, 0, 0, 0, 65536, 0),
     (512, 32, 65536, 5, 1, 1, 65536, 65536), {}, {}),
    ('cube', [0x81000002, 0x1F900094, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 1, 20, 2), (6, 0, 0, 0, 98304, 0),
     (512, 32, 16384, 5, 1, 1, 16384, 98304), {}, {}),
    ('2d', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 24, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 32, 4096, 1, 1, 1, 2, 2), {}, {}),
    ('3d', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 24, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 32, 8192, 1, 1, 1, 2, 2), {}, {}),
    ('cube', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 24, 2), (6, 0, 4294967295, 0, 20482, 0),
     (64, 32, 4096, 1, 1, 1, 2, 20482), {}, {}),
    ('1d', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 24, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 1, 4096, 1, 1, 1, 2, 2), {}, {}),
    ('2d', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 24, 2), (1, 0, 0, 0, 34, 0),
     (64, 32, 4096, 17, 1, 1, 34, 34), {}, {}),
    ('3d', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 24, 2), (1, 0, 0, 0, 34, 0),
     (64, 32, 8192, 17, 1, 1, 34, 34), {}, {}),
    ('cube', [0x00400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 24, 2), (6, 0, 0, 0, 20514, 0),
     (64, 32, 4096, 17, 1, 1, 34, 20514), {}, {}),
    ('2d', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 24, 2), (1, 0, 4294967295, 0, 3072, 0),
     (64, 32, 4096, 1, 1, 1, 3072, 3072), {}, {}),
    ('3d', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 24, 2), (1, 0, 4294967295, 0, 8192, 0),
     (64, 32, 8192, 1, 1, 1, 8192, 8192), {}, {}),
    ('cube', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 24, 2), (6, 0, 4294967295, 0, 23552, 0),
     (64, 32, 4096, 1, 1, 1, 3072, 23552), {}, {}),
    ('2d', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 24, 2), (1, 0, 0, 0, 3072, 0),
     (64, 32, 4096, 17, 1, 1, 3072, 3072), {}, {}),
    ('3d', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 24, 2), (1, 0, 0, 0, 8192, 0),
     (64, 32, 8192, 17, 1, 1, 8192, 8192), {}, {}),
    ('cube', [0x80400002, 0x1F900098, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 24, 2), (6, 0, 0, 0, 23552, 0),
     (64, 32, 4096, 17, 1, 1, 3072, 23552), {}, {}),
    ('2d', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 26, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 32, 8192, 1, 1, 1, 8, 8), {}, {}),
    ('3d', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 26, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 32, 32768, 1, 1, 1, 8, 8), {}, {}),
    ('cube', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 26, 2), (6, 0, 4294967295, 0, 40968, 0),
     (256, 32, 8192, 1, 1, 1, 8, 40968), {}, {}),
    ('1d', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 26, 2), (1, 0, 4294967295, 0, 8, 0),
     (256, 1, 4096, 1, 1, 1, 8, 8), {}, {}),
    ('2d', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 26, 2), (1, 0, 0, 0, 136, 0),
     (256, 32, 8192, 17, 1, 1, 136, 136), {}, {}),
    ('3d', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 26, 2), (1, 0, 0, 0, 136, 0),
     (256, 32, 32768, 17, 1, 1, 136, 136), {}, {}),
    ('cube', [0x00400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 26, 2), (6, 0, 0, 0, 41096, 0),
     (256, 32, 8192, 17, 1, 1, 136, 41096), {}, {}),
    ('2d', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 26, 2), (1, 0, 4294967295, 0, 8192, 0),
     (256, 32, 8192, 1, 1, 1, 8192, 8192), {}, {}),
    ('3d', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 26, 2), (1, 0, 4294967295, 0, 32768, 0),
     (256, 32, 32768, 1, 1, 1, 32768, 32768), {}, {}),
    ('cube', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 26, 2), (6, 0, 4294967295, 0, 49152, 0),
     (256, 32, 8192, 1, 1, 1, 8192, 49152), {}, {}),
    ('2d', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 26, 2), (1, 0, 0, 0, 8192, 0),
     (256, 32, 8192, 17, 1, 1, 8192, 8192), {}, {}),
    ('3d', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 26, 2), (1, 0, 0, 0, 32768, 0),
     (256, 32, 32768, 17, 1, 1, 32768, 32768), {}, {}),
    ('cube', [0x80400002, 0x1F90009A, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 26, 2), (6, 0, 0, 0, 49152, 0),
     (256, 32, 8192, 17, 1, 1, 8192, 49152), {}, {}),
    ('2d', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 36, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('3d', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 36, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 16384, 1, 1, 1, 4, 4), {}, {}),
    ('cube', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 36, 2), (6, 0, 4294967295, 0, 20484, 0),
     (128, 32, 4096, 1, 1, 1, 4, 20484), {}, {}),
    ('1d', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 36, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 1, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('2d', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 36, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 4096, 17, 1, 1, 68, 68), {}, {}),
    ('3d', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 36, 2), (1, 0, 0, 0, 68, 0),
     (128, 32, 16384, 17, 1, 1, 68, 68), {}, {}),
    ('cube', [0x00400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 36, 2), (6, 0, 0, 0, 20548, 0),
     (128, 32, 4096, 17, 1, 1, 68, 20548), {}, {}),
    ('2d', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 36, 2), (1, 0, 4294967295, 0, 4096, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 36, 2), (1, 0, 4294967295, 0, 16384, 0),
     (128, 32, 16384, 1, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 36, 2), (6, 0, 4294967295, 0, 24576, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 36, 2), (1, 0, 0, 0, 4096, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 36, 2), (1, 0, 0, 0, 16384, 0),
     (128, 32, 16384, 17, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x80400002, 0x1F9000A4, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 36, 2), (6, 0, 0, 0, 24576, 0),
     (128, 32, 4096, 17, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 0, 44, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('3d', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 0, 44, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 32, 16384, 1, 1, 1, 4, 4), {}, {}),
    ('cube', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 0, 44, 2), (6, 0, 4294967295, 0, 20484, 0),
     (128, 32, 4096, 1, 1, 1, 4, 20484), {}, {}),
    ('1d', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 4, 0, 0, 0, 44, 2), (1, 0, 4294967295, 0, 4, 0),
     (128, 1, 4096, 1, 1, 1, 4, 4), {}, {}),
    ('2d', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 1, 44, 2), (1, 0, 0, 0, 20, 0),
     (128, 32, 4096, 5, 1, 1, 20, 20), {}, {}),
    ('3d', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 1, 44, 2), (1, 0, 0, 0, 20, 0),
     (128, 32, 16384, 5, 1, 1, 20, 20), {}, {}),
    ('cube', [0x01000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 0, 1, 44, 2), (6, 0, 0, 0, 20500, 0),
     (128, 32, 4096, 5, 1, 1, 20, 20500), {}, {}),
    ('2d', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 0, 44, 2), (1, 0, 4294967295, 0, 4096, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 0, 44, 2), (1, 0, 4294967295, 0, 16384, 0),
     (128, 32, 16384, 1, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 0, 44, 2), (6, 0, 4294967295, 0, 24576, 0),
     (128, 32, 4096, 1, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 1, 1, 44, 2), (1, 0, 0, 0, 4096, 0),
     (128, 32, 4096, 5, 1, 1, 4096, 4096), {}, {}),
    ('3d', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 1, 1, 44, 2), (1, 0, 0, 0, 16384, 0),
     (128, 32, 16384, 5, 1, 1, 16384, 16384), {}, {}),
    ('cube', [0x81000002, 0x1F9000AC, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 4, 0, 1, 1, 44, 2), (6, 0, 0, 0, 24576, 0),
     (128, 32, 4096, 5, 1, 1, 4096, 24576), {}, {}),
    ('2d', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 0, 47, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 32, 4096, 1, 1, 1, 2, 2), {}, {}),
    ('3d', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 0, 47, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 32, 8192, 1, 1, 1, 2, 2), {}, {}),
    ('cube', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 0, 47, 2), (6, 0, 4294967295, 0, 20482, 0),
     (64, 32, 4096, 1, 1, 1, 2, 20482), {}, {}),
    ('1d', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000000],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 0, 0, 0, 0, 1, 0, 0, 0, 47, 2), (1, 0, 4294967295, 0, 2, 0),
     (64, 1, 4096, 1, 1, 1, 2, 2), {}, {}),
    ('2d', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 0, 1, 47, 2), (1, 0, 0, 0, 34, 0),
     (64, 32, 4096, 17, 1, 1, 34, 34), {}, {}),
    ('3d', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 0, 1, 47, 2), (1, 0, 0, 0, 34, 0),
     (64, 32, 8192, 17, 1, 1, 34, 34), {}, {}),
    ('cube', [0x00400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 0, 1, 47, 2), (6, 0, 0, 0, 20514, 0),
     (64, 32, 4096, 17, 1, 1, 34, 20514), {}, {}),
    ('2d', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 0, 47, 2), (1, 0, 4294967295, 0, 3072, 0),
     (64, 32, 4096, 1, 1, 1, 3072, 3072), {}, {}),
    ('3d', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 0, 47, 2), (1, 0, 4294967295, 0, 8192, 0),
     (64, 32, 8192, 1, 1, 1, 8192, 8192), {}, {}),
    ('cube', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000600],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 0, 47, 2), (6, 0, 4294967295, 0, 23552, 0),
     (64, 32, 4096, 1, 1, 1, 3072, 23552), {}, {}),
    ('2d', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000A00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 1, 0, 1, 1, 47, 2), (1, 0, 0, 0, 3072, 0),
     (64, 32, 4096, 17, 1, 1, 3072, 3072), {}, {}),
    ('3d', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000C00],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 1, 0, 1, 1, 47, 2), (1, 0, 0, 0, 8192, 0),
     (64, 32, 8192, 17, 1, 1, 8192, 8192), {}, {}),
    ('cube', [0x80400002, 0x1F9000AF, 0x00000000, 0x00A80D10, 0x00000000, 0x00000E00],
     (0, 0, 5, 129280, 0, 0, 0), (129280, 0, 3, 0, 0, 5, 1, 0, 1, 1, 47, 2), (6, 0, 0, 0, 23552, 0),
     (64, 32, 4096, 17, 1, 1, 3072, 23552), {}, {}),
    ('2d', [0x01000002, 0x1F9000B1, 0x00000000, 0x00A80D10, 0x00000000, 0x00000200],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 1, 0, 0, 0, 4, 0, 0, 0, 49, 2), (1, 0, 4294967295, 0, 16, 0),
     (512, 32, 16384, 1, 1, 1, 16, 16), {}, {}),
    ('3d', [0x01000002, 0x1F9000B1, 0x00000000, 0x00A80D10, 0x00000000, 0x00000400],
     (0, 0, 0, 129280, 0, 0, 0), (129280, 0, 2, 0, 0, 0, 4, 0, 0, 0, 49, 2), (1, 0, 4294967295, 0, 16, 0),
     (512, 32, 65536, 1, 1, 1, 16, 16), {}, {}),
]


@test('T3B math and tables: xe_log2_ceil / xe_next_pow2 in uint32 (0 -> 32 / 0), the 64-row block-geometry table row for row with FORMAT_TABLE (2x1 REP formats, 4x4 DXT/DXN/CTX1, 4x1 k_32_AS_8, 2x1 k_32_AS_8_8, 4x1 k_32_AS_8_INTERLACED), bytes per block (k_1 -> 0), the eleven GetBaseFormat remaps by name, the pinned constants and statuses')
def _():
    eq([X.xe_log2_ceil(v) for v in (0, 1, 2, 3, 4, 5, 17, 360, 0x80000000, 0x80000001)], [32, 0, 1, 2, 2, 3, 5, 9, 31, 32], 'log2_ceil (math.h L325-L330)')
    eq([X.xe_next_pow2(v) for v in (0, 1, 2, 3, 17, 100, 360, 0x80000000, 0x80000001)], [0, 1, 2, 4, 32, 128, 512, 0x80000000, 0], 'next_pow2 (L97-L106)')
    eq(len(X.FORMAT_BLOCK_TABLE), 64, '64 rows')
    eq([X.FORMAT_BLOCK_TABLE[i] for i in (0, 1, 2, 6, 11, 12, 18, 19, 20, 24, 36, 39, 40, 44, 49, 51, 52, 53, 56, 58, 59, 60, 61, 63)],
       [(1, 1), (1, 1), (1, 1), (1, 1), (2, 1), (2, 1), (4, 4), (4, 4), (4, 4), (1, 1), (1, 1), (4, 1), (2, 1), (4, 1), (4, 4), (4, 4), (4, 4), (4, 4),
        (1, 1), (4, 4), (4, 4), (4, 4), (4, 4), (1, 1)], 'block rows (texture_info_formats.inl)')
    eq([X.FORMAT_TABLE[i][0] for i in (11, 12, 18, 39, 40, 44, 58, 61)],
       ['k_Cr_Y1_Cb_Y0_REP', 'k_Y1_Cr_Y0_Cb_REP', 'k_DXT1', 'k_32_AS_8', 'k_32_AS_8_8', 'k_32_AS_8_INTERLACED', 'k_DXT3A', 'k_DXT3A_AS_1_1_1_1'], 'row names')
    eq([X.FORMAT_BLOCK_TABLE[F[n]] for n in ('k_DXN', 'k_CTX1', 'k_DXT5A', 'k_DXT1_AS_16_16_16_16', 'k_8_8_8_8_GAMMA_EDRAM')], [(4, 4), (4, 4), (4, 4), (4, 4), (1, 1)],
       'block rows by name')
    eq([X.format_block_info(f) for f in (F['k_DXT1'], F['k_DXT4_5'], F['k_8_8_8_8'], F['k_8'], F['k_32_AS_8'], F['k_32_AS_8_8'], F['k_1'], F['k_1_REVERSE'],
                                         F['k_Cr_Y1_Cb_Y0_REP'], F['k_DXN'], F['k_16_16_16_16_FLOAT'])],
       [(4, 4, 4, 8), (4, 4, 8, 16), (1, 1, 32, 4), (1, 1, 8, 1), (4, 1, 8, 4), (2, 1, 16, 4), (1, 1, 1, 0), (1, 1, 1, 0), (2, 1, 16, 4), (4, 4, 8, 16), (1, 1, 64, 8)],
       'bytes per block = bw * bh * bpp / 8')
    eq({X.FORMAT_TABLE[a][0]: X.FORMAT_TABLE[b][0] for a, b in X.BASE_FORMAT_REMAP.items()},
       {'k_16_EXPAND': 'k_16_FLOAT', 'k_16_16_EXPAND': 'k_16_16_FLOAT', 'k_16_16_16_16_EXPAND': 'k_16_16_16_16_FLOAT',
        'k_8_8_8_8_AS_16_16_16_16': 'k_8_8_8_8', 'k_DXT1_AS_16_16_16_16': 'k_DXT1', 'k_DXT2_3_AS_16_16_16_16': 'k_DXT2_3',
        'k_DXT4_5_AS_16_16_16_16': 'k_DXT4_5', 'k_2_10_10_10_AS_16_16_16_16': 'k_2_10_10_10', 'k_10_11_11_AS_16_16_16_16': 'k_10_11_11',
        'k_11_11_10_AS_16_16_16_16': 'k_11_11_10', 'k_8_8_8_8_GAMMA_EDRAM': 'k_8_8_8_8'}, 'GetBaseFormat (texture_info.h L27-L60)')
    eq((X.GPU_ALLOW_INVALID_FETCH_CONSTANTS, X.TEXTURE_MAX_MIPS, X.TEXTURE_SUBRESOURCE_ALIGNMENT, X.TEXTURE_LINEAR_ROW_ALIGNMENT,
        X.TEXTURE_2D_CUBE_MAX_WIDTH_HEIGHT, X.PHYSICAL_PAGE_MASK, X.TEXTURE_BACKING_VERSION),
       (True, 14, 4096, 256, 8192, 0x1FFFF, 'xtr_draws-texture-backing/1'), 'constants (gpu_flags.cc L29-L34; xenos.h L1177-L1226; texture_util.cc L79-L80)')
    eq((X.BACKING_STATUSES, X.POST_STATUSES),
       (('complete', 'invalid_type', 'no_data', 'invalid_1d', 'unsupported_format'),
        ('translated', 'translation_invalid', 'analysis_mismatch', 'not_in_table', 'no_table', 'ucode_unmapped', 'missing')), 'statuses')
    eq(X.mask_slots(0x12), [1, 4], 'mask -> slots')
    eq((X.mask_slots(0), X.mask_slots(None), X.mask_slots(0x80000001)), ([], [], [0, 31]), 'mask edge cases')


@test('T3B packed mips by hand (texture_util.h L41-L44; texture_util.cc L116-L206): packed level 0 at 16x16, 1 at 17x17 and 32x32, 5 at 512x512 and 640x360; offsets of a square 512 tail (16, 8, 4 blocks on x, then 8 and 4 on y), a wide 256x128 tail (rows on y, then columns on x), DXT block scaling, and the 3D depth blocks (4, or 4 per doubling)')
def _():
    eq([X.get_packed_mip_level(w, h) for w, h in ((16, 16), (17, 17), (32, 32), (512, 512), (640, 360), (1, 1), (256, 128), (8, 4096))],
       [0, 1, 1, 5, 5, 0, 3, 0], 'packed level')
    eq([X.get_packed_mip_offset(512, 512, 1, F['k_8_8_8_8'], m) for m in (4, 5, 6, 7, 8, 9)],
       [(False, 0, 0, 0), (True, 16, 0, 0), (True, 8, 0, 0), (True, 4, 0, 0), (True, 0, 8, 0), (True, 0, 4, 0)], 'square 512 tail')
    eq(X.get_packed_mip_offset(512, 512, 1, F['k_DXT1'], 5), (True, 4, 0, 0), 'DXT: block offsets are texel offsets >> log2(block)')
    eq([X.get_packed_mip_offset(256, 128, 1, F['k_8_8_8_8'], m) for m in (3, 4, 6, 7)],
       [(True, 0, 16, 0), (True, 0, 8, 0), (True, 16, 0, 0), (True, 8, 0, 0)], 'wide tail: the first three packed mips stack on y, later ones on x')
    eq([X.get_packed_mip_offset(16, 16, d, F['k_8_8_8_8'], 5) for d in (16, 256)], [(True, 0, 2, 4), (True, 0, 2, 12)], '3D depth blocks')


@test('T3B GetSubresourcesFromFetchConstant by hand (texture_util.cc L23-L114): size views per dimension (1D 24-bit width, stacked depth, 3D depth in the longest axis, cube 6 faces), 17-bit physical pages, mip-page 0 forcing 0..0, min/max clamped to the size level, a set mip page with max 0 dropped, base dropped when the range starts above 0, base 0 raising min to 1')
def _():
    def sub(**kw):
        return list(X.get_subresources_from_fetch(X.decode_tfetch(tfc(**kw))).values())
    eq(sub(dim=1, w=640, h=360, base=0x1F900000), [639, 359, 0, 0x1F900, 0, 0, 0, 9], '2D no mips: size level log2_floor(640) = 9, mip page 0')
    eq(sub(dim=1, w=640, h=360, mipbase=0x11100000, mipmax=13), [639, 359, 0, 0x1F900, 0x11100, 0, 9, 9], 'max clamped to the size level')
    eq(sub(dim=1, w=16, h=16, mipbase=0x11100000, mipmin=13, mipmax=13), [15, 15, 0, 0, 0x11100, 4, 4, 4], 'min clamped to 4; range 4..4 starts above 0 -> base page dropped')
    eq(sub(dim=1, w=640, h=360, mipbase=0x11100000, mipmin=3, mipmax=1), [639, 359, 0, 0, 0x11100, 3, 3, 9], 'max raised to min; base dropped')
    eq(sub(dim=1, w=640, h=360, base=0, mipbase=0x11100000, mipmax=5), [639, 359, 0, 0, 0x11100, 1, 5, 9], 'base 0: min raised to 1')
    eq(sub(dim=1, w=640, h=360, mipbase=0x11100000, mipmax=0), [639, 359, 0, 0x1F900, 0, 0, 0, 9], 'mip page set but max 0 -> mip page dropped')
    eq(sub(dim=1, w=640, h=360, base=0xFFFFF000, mipbase=0xFFFFF000, mipmax=9), [639, 359, 0, 0x1FFFF, 0x1FFFF, 0, 9, 9], '20-bit page fields masked to 17 bits')
    eq(sub(dim=1, w=640, h=360, base=0x20001000, mipbase=0x20002000, mipmax=9), [639, 359, 0, 1, 2, 0, 9, 9], 'bit 17 dropped')
    eq(sub(dim=1, w=32, h=32, d=6, stacked=1), [31, 31, 5, 0x1F900, 0, 0, 0, 5], 'stacked: depth from the 2D view')
    eq(sub(dim=1, w=32, h=32, d=6, stacked=0), [31, 31, 0, 0x1F900, 0, 0, 0, 5], 'not stacked: depth 0')
    eq(sub(dim=2, w=64, h=32, d=33, mipbase=0x11100000, mipmax=13), [63, 31, 32, 0x1F900, 0x11100, 0, 6, 6], '3D: longest axis includes depth (log2_floor(64) = 6)')
    eq(sub(dim=2, w=4, h=4, d=64, mipbase=0x11100000, mipmax=13), [3, 3, 63, 0x1F900, 0x11100, 0, 6, 6], '3D: depth 64 is the longest axis')
    eq(sub(dim=3, w=128, h=128, mipbase=0x11100000, mipmax=13), [127, 127, 5, 0x1F900, 0x11100, 0, 7, 7], 'cube: 6 faces, depth not in the longest axis')
    eq(sub(dim=0, w=1 << 24), [0xFFFFFF, 0, 0, 0x1F900, 0, 0, 0, 24], '1D: 24-bit width')


@test('T3B BindingInfoFromFetchConstant gate by hand (texture_cache.cc L871-L965): kTexture and, under the pinned default cvar, kInvalidTexture are accepted (cvar off refuses it); kInvalidVertex / kVertex are invalid by name; both pages 0 = no data; 1D >= 8192 wide, tiled or packed is invalid (all three named); GetBaseFormat applied to the key with the raw format kept; key fields')
def _():
    def bi(raw, **kw):
        st, reason, key, sub = X.binding_info_from_fetch(X.decode_tfetch(raw), **kw)
        return st, reason, key
    eq(bi(TFC_1)[0], 'valid', 'kTexture')
    st, reason, key = bi(tfc(type_=0))
    eq(st, 'valid', 'kInvalidTexture under gpu_allow_invalid_fetch_constants=true (the pinned default)')
    st, reason, key = bi(tfc(type_=0), allow_invalid=False)
    eq((st, key), ('invalid_type', None), 'kInvalidTexture refused when the cvar is off')
    ck('gpu_allow_invalid_fetch_constants' in reason, reason)
    for t, name in ((1, 'kInvalidVertex'), (3, 'kVertex')):
        st, reason, key = bi(tfc(type_=t))
        eq((st, key), ('invalid_type', None), name)
        ck('%s(%d)' % (name, t) in reason and 'completely invalid' in reason, reason)
    st, reason, key = bi(TFC_NODATA)
    eq((st, key), ('no_data', None), 'both pages 0')
    ck('base and mip pages both 0' in reason, reason)
    eq(bi(tfc(dim=0, w=8192, tiled=0))[0], 'valid', '1D 8192 wide is the last valid width')
    st, reason, key = bi(tfc(dim=0, w=8193, tiled=0))
    eq((st, key), ('invalid_1d', None), '1D 8193 wide')
    ck('too wide (8193)' in reason, reason)
    eq(bi(tfc(dim=0, w=256, tiled=1))[:1], ('invalid_1d',), '1D tiled')
    eq(bi(tfc(dim=0, w=256, tiled=0, packed=1))[:1], ('invalid_1d',), '1D packed')
    st, reason, key = bi(tfc(dim=0, w=9000, tiled=1, packed=1))
    eq(reason.count(';'), 2, 'all three 1D defects named: %r' % reason)
    eq(bi(tfc(dim=1, w=8193, h=1, tiled=1, packed=1))[0], 'valid', 'the 1D checks apply to 1D only')
    st, reason, key = bi(tfc(fmt=F['k_DXT1_AS_16_16_16_16'], w=512, h=512, pitch=16))
    eq((st, key['format'], key['format_name'], key['raw_format']), ('valid', F['k_DXT1'], 'k_DXT1', F['k_DXT1_AS_16_16_16_16']), 'GetBaseFormat on the key')
    st, reason, key = bi(TFC_1)
    eq(list(key), ['base_page', 'mip_page', 'dimension', 'dimension_name', 'width_minus_1', 'height_minus_1', 'depth_or_array_size_minus_1',
                   'pitch', 'pitch_texels', 'mip_max_level', 'tiled', 'packed_mips', 'format', 'format_name', 'raw_format', 'endianness'], 'key fields')
    eq((key['base_page'], key['mip_page'], key['dimension_name'], key['width_minus_1'], key['height_minus_1'], key['pitch'], key['pitch_texels'],
        key['mip_max_level'], key['tiled'], key['packed_mips'], key['format_name'], key['endianness']),
       (0x11060, 0x110A0, 'k2DOrStacked', 511, 511, 16, 512, 9, 1, 1, 'k_DXT4_5', 2), 'TFC_1 key')


@test('T3B GetGuestTextureLayout branches by hand (texture_util.cc L208-L448): linear 2D base (640x360 k_8), pow2 mip pitch/rows (k_8_8_8_8 mip1 2048/256), linear mip rows aligned to 256 (k_8 mip3), 3D depth aligned to 4, cube array 6, stacked array 6, packed level 0 (mips[0] written; none when max level 0), a packed tail (title constant), no base, all-zero dims; the undefined-in-pin fields are null and named')
def _():
    Z = dict(X.ZERO_LEVEL)

    def lay(raw, **kw):
        return X.derive_texture_backing(raw if isinstance(raw, list) else tfc(**kw))['layout']

    def lv(L, i=None):
        return list((L['base'] if i is None else L['mips'][i]).values())
    L = lay(None, fmt=F['k_8'], dim=1, w=640, h=360, pitch=24, tiled=0)
    eq((lv(L), L['array_size'], L['max_level'], L['packed_level'], L['mips_total_extent_bytes']),
       ([768, 384, 294912, 640, 360, 1, 276352, 276352], 1, 0, None, 0), 'linear 2D base: 768*359 + 640')
    eq((L['mips'][0], L['mips'][13], L['mip_offsets_bytes'][0], L['mip_offsets_bytes'][1], L['undefined_in_pin']),
       (Z, Z, None, 0, ['mip_offsets_bytes[0]']), 'not packed: mips[0] zeroed, mip_offsets[0] never written')
    L = lay(None, fmt=F['k_8_8_8_8'], dim=1, w=640, h=360, pitch=20, tiled=0, mipbase=0x11100000, mipmax=3)
    eq(lv(L), [2560, 384, 983040, 640, 360, 1, 921600, 921600], 'k_8_8_8_8 base')
    eq(lv(L, 1), [2048, 256, 524288, 320, 180, 1, 367872, 367872], 'mip1: pitch from next_pow2(640) >> 1 = 512 texels, rows next_pow2(360) >> 1 = 256')
    eq(lv(L, 2), [1024, 128, 131072, 160, 90, 1, 91776, 91776], 'mip2')
    eq(lv(L, 3), [512, 64, 32768, 80, 45, 1, 22848, 22848], 'mip3')
    eq((L['mip_offsets_bytes'][:5], L['mips_total_extent_bytes'], L['max_level']), ([None, 0, 524288, 655360, 0], 678208, 3), 'offsets accumulate stride * array_size; total = last offset + extent')
    L = lay(None, fmt=F['k_8'], dim=1, w=640, h=360, pitch=24, tiled=0, mipbase=0x11100000, mipmax=3)
    eq((lv(L, 1), lv(L, 2), lv(L, 3)), ([512, 256, 131072, 320, 180, 1, 91968, 91968], [256, 128, 32768, 160, 90, 1, 22944, 22944],
                                          [256, 64, 16384, 80, 45, 1, 11344, 11344]), 'k_8 mips: mip3 row 128 aligned up to 256 (linear only)')
    eq((L['mip_offsets_bytes'][1:4], L['mips_total_extent_bytes']), ([0, 131072, 163840], 175184), 'k_8 offsets')
    L = lay(None, fmt=F['k_8'], dim=2, w=16, h=16, d=5, pitch=4, tiled=0)
    eq((lv(L), L['array_size']), ([128, 32, 32768, 16, 16, 5, 18320, 18320], 1), '3D linear: slice stride 4096 * align(5, 4) = 32768; extent 4096*4 + 128*15 + 16')
    L = lay(None, fmt=F['k_8_8_8_8'], dim=3, w=32, h=32, pitch=1, tiled=0)
    eq((lv(L), L['array_size']), ([128, 32, 4096, 32, 32, 1, 4096, 24576], 6), 'cube: 6 array slices, level extent = 5 strides + one slice')
    L = lay(None, fmt=F['k_8'], dim=1, w=32, h=32, d=6, stacked=1, pitch=1, tiled=0)
    eq((lv(L), L['array_size']), ([32, 32, 4096, 32, 32, 1, 1024, 21504], 6), 'stacked: array size from the 2D view depth')
    L = lay(None, fmt=F['k_8'], dim=1, w=32, h=32, d=6, stacked=0, pitch=1, tiled=0)
    eq((lv(L)[7], L['array_size']), (1024, 1), 'not stacked: one slice')
    L = lay(None, fmt=F['k_8_8_8_8'], dim=1, w=16, h=16, pitch=1, tiled=1, packed=1, mipbase=0x11100000, mipmax=4)
    eq((L['packed_level'], L['max_level'], L['base']['x_extent_blocks'], L['base']['y_extent_blocks'], L['base']['z_extent']), (0, 4, 32, 16, 1),
       'packed level 0: the base extent covers the tail (x 16 + 16 blocks)')
    eq(L['base']['array_slice_data_extent_bytes'], X.tiled_upper_bound_2d(32, 16, 32, 2), 'tiled base extent = pinned upper bound')
    m0 = L['mips'][0]
    eq((m0['row_pitch_bytes'], m0['z_slice_stride_block_rows'], m0['array_slice_stride_bytes'], m0['x_extent_blocks'], m0['y_extent_blocks'], m0['z_extent']),
       (128, 32, 4096, 32, 16, 1), 'mips[0] holds the whole packed tail (levels 0..4)')
    eq((L['mip_offsets_bytes'][0], L['mips_total_extent_bytes'], L['mips'][1], L['undefined_in_pin']), (0, m0['level_data_extent_bytes'], Z, []),
       'packed level 0 with mips: everything written')
    L = lay(None, fmt=F['k_8_8_8_8'], dim=1, w=16, h=16, pitch=1, tiled=1, packed=1)
    eq((L['packed_level'], L['max_level'], L['mips'][0], L['mip_offsets_bytes'][0], L['undefined_in_pin']), (0, 0, None, None, ['mips[0]', 'mip_offsets_bytes[0]']),
       'packed level 0 without mips: the pin writes neither mips[0] nor mip_offsets[0]')
    L = lay(TITLE_TFC)
    eq((L['packed_level'], L['max_level'], lv(L), lv(L, 1), lv(L, 5), L['mips'][6], L['mip_offsets_bytes'][:7], L['mips_total_extent_bytes'], L['undefined_in_pin']),
       (5, 9, [2048, 128, 262144, 128, 128, 1, 262144, 262144], [1024, 64, 65536, 64, 64, 1, 65536, 65536], [512, 32, 16384, 8, 4, 1, 16384, 16384], Z,
        [None, 0, 65536, 81920, 98304, 114688, 0], 131072, ['mip_offsets_bytes[0]']), 'title constant: packed tail at level 5 (8x4 blocks), mips 131072 B')
    d = X.derive_texture_backing(tfc(fmt=F['k_8'], dim=1, w=640, h=360, pitch=24, tiled=0, base=0, mipbase=0x11100000, mipmax=3))
    eq((d['status'], d['layout']['base'], d['base_interval'], d['mips_interval']['start'], d['mips_interval']['length'], d['join_usable']),
       ('complete', Z, None, 0x11100000, 175184, True), 'no base: base level zeroed, only the mip interval')
    L = X.guest_texture_layout(1, 1, 16, 16, 1, 0, F['k_8'], 0, True, 13)
    eq((L['max_level'], L['mips'][4]['x_extent_blocks'], L['mips'][5], L['mip_offsets_bytes'][5]), (4, 1, Z, 0), 'direct call: max level clamped to log2_floor(16) = 4 (L244-L248)')
    L = X.guest_texture_layout(1, 1, 0, 1, 1, 0, F['k_8'], 0, True, 0)
    eq((L['array_size'], L['base'], L['mips'], L['mip_offsets_bytes'], L['undefined_in_pin'], L['packed_level']), (0, Z, [Z] * 14, [0] * 14, [], 0), 'all-zero dims (L236-L241)')


@test('T3B3 fixed pinned-layout oracle vectors: %d constants (every trace class, all dimensions, linear/tiled, stacked, packed, separate mip bases, base/mip page 0, all 64 formats, pitches, mip bounds, types) reproduce the ACTUAL pinned GetSubresourcesFromFetchConstant / BindingInfoFromFetchConstant / GetGuestTextureLayout field for field; the pin-undefined fields are null' % len(LAYOUT_ORACLE_VECTORS))
def _():
    n_layout = 0
    for cls, raw, sub, key, hdr, base, mips, offs in LAYOUT_ORACLE_VECTORS:
        t = X.decode_tfetch(raw)
        got = list(X.get_subresources_from_fetch(t).values())[:7]
        eq(got, list(sub), '%s %s subresources' % (cls, fkey(raw)))
        d = X.derive_texture_backing(raw)
        valid = d['status'] in ('complete', 'unsupported_format')
        eq(valid, key is not None, '%s %s key validity (status %s)' % (cls, fkey(raw), d['status']))
        if key is None:
            eq((d['key'], d['layout'], d['base_interval'], d['mips_interval'], d['join_usable']), (None, None, None, None, False), '%s: nothing invented' % fkey(raw))
            continue
        k = d['key']
        eq([k['base_page'], k['mip_page'], k['dimension'], k['width_minus_1'], k['height_minus_1'], k['depth_or_array_size_minus_1'], k['pitch'],
            k['mip_max_level'], k['tiled'], k['packed_mips'], k['format'], k['endianness']], list(key), '%s %s key' % (cls, fkey(raw)))
        if d['status'] == 'unsupported_format':
            eq((d['format']['bytes_per_block'], d['layout']), (0, None), '%s: 0 bytes per block -> no layout' % fkey(raw))
            continue
        n_layout += 1
        L = d['layout']
        pl = 0xFFFFFFFF if L['packed_level'] is None else L['packed_level']
        eq((L['array_size'], L['max_level'], pl, L['mips_total_extent_bytes'], L['base']['level_data_extent_bytes'], L['mips_total_extent_bytes']), tuple(hdr),
           '%s %s header' % (cls, fkey(raw)))
        eq(tuple(L['base'].values()), tuple(base), '%s %s base' % (cls, fkey(raw)))
        for i in range(14):
            m = L['mips'][i]
            undefined = (i == 0 and pl == 0 and L['max_level'] == 0)
            if undefined:
                eq(m, None, '%s mips[0] undefined in the pin' % fkey(raw))
            else:
                eq(tuple(m.values()), tuple(mips.get(i, (0,) * 8)), '%s %s mip%d' % (cls, fkey(raw), i))
            off = L['mip_offsets_bytes'][i]
            if i == 0 and not (pl == 0 and L['max_level'] != 0):
                eq(off, None, '%s mip_offsets[0] undefined in the pin (written only for a level-0 packed tail with mips)' % fkey(raw))
            else:
                eq(off, offs.get(i, 0), '%s %s mip_offsets[%d]' % (cls, fkey(raw), i))
        eq(d['join_usable'], bool((d['base_interval'] and d['base_interval']['length']) or (d['mips_interval'] and d['mips_interval']['length'])), 'join_usable')
    ck(n_layout >= 100, 'layouts compared: %d' % n_layout)


@test('T3B derive_texture_backing end to end: the title constant gives SEPARATE base [11060000,+262144) and mip [110A0000,+131072) intervals; an FMV plane only a base interval; no-data / invalid type / unsupported format / invalid 1D are named with no interval and join_usable false; the 17-bit page puts a 0xFFFFF page at 1FFFF000; allowed_by_cvar marks type 0')
def _():
    d = X.derive_texture_backing(TITLE_TFC)
    eq(list(d), ['backing_version', 'status', 'reason', 'join_usable', 'fetch_constant_type', 'fetch_constant_type_name', 'allowed_by_cvar', 'subresources',
                 'key', 'format', 'layout', 'base_interval', 'mips_interval'], 'fields')
    eq((d['backing_version'], d['status'], d['reason'], d['join_usable'], d['fetch_constant_type_name'], d['allowed_by_cvar']),
       ('xtr_draws-texture-backing/1', 'complete', None, True, 'kTexture', False), 'title status')
    eq((dict(d['base_interval']), dict(d['mips_interval'])), (dict(start=0x11060000, length=262144, end_exclusive=0x110A0000),
                                                             dict(start=0x110A0000, length=131072, end_exclusive=0x110C0000)), 'separate base and mip intervals')
    eq(dict(d['format']), dict(block_width=4, block_height=4, bits_per_pixel=8, bytes_per_block=16, bytes_per_block_log2=4), 'DXT4_5 geometry')
    eq((d['subresources']['base_page'], d['subresources']['mip_page'], d['key']['pitch_texels']), (0x11060, 0x110A0, 512), 'pages and pitch')
    d = X.derive_texture_backing(FMV_TFC)
    eq((d['status'], dict(d['base_interval']), d['mips_interval'], d['join_usable'], d['key']['format_name'], d['key']['tiled']),
       ('complete', dict(start=0x0E2C0000, length=276352, end_exclusive=0x0E2C0000 + 276352), None, True, 'k_8', 0), 'FMV plane')
    d = X.derive_texture_backing(TFC_NODATA)
    eq((d['status'], d['key'], d['format'], d['layout'], d['base_interval'], d['mips_interval'], d['join_usable'], d['subresources']['base_page']),
       ('no_data', None, None, None, None, None, False, 0), 'no data')
    d = X.derive_texture_backing(tfc(type_=3))
    eq((d['status'], d['subresources'], d['key'], d['layout'], d['join_usable'], d['fetch_constant_type_name']), ('invalid_type', None, None, None, False, 'kVertex'), 'kVertex')
    d = X.derive_texture_backing(tfc(type_=0))
    eq((d['status'], d['allowed_by_cvar'], d['join_usable']), ('complete', True, True), 'kInvalidTexture accepted under the pinned default and flagged')
    d = X.derive_texture_backing(tfc(fmt=F['k_1'], w=64, h=64, pitch=2))
    eq((d['status'], d['key']['format_name'], d['format']['bytes_per_block'], d['format']['bytes_per_block_log2'], d['layout'], d['base_interval'], d['join_usable']),
       ('unsupported_format', 'k_1', 0, None, None, None, False), 'k_1: 0 bytes per block is fail-closed, key kept')
    ck('log2_floor(0)' in d['reason'], d['reason'])
    d = X.derive_texture_backing(tfc(dim=0, w=256, tiled=1))
    eq((d['status'], d['key'], d['layout'], d['join_usable']), ('invalid_1d', None, None, False), '1D tiled')
    d = X.derive_texture_backing(tfc(fmt=F['k_8'], dim=1, w=64, h=64, pitch=2, tiled=0, base=0xFFFFF000, mipbase=0xFFFFE000, mipmax=2))
    eq((d['key']['base_page'], d['base_interval']['start'], d['key']['mip_page'], d['mips_interval']['start']), (0x1FFFF, 0x1FFFF000, 0x1FFFE, 0x1FFFE000),
       '17-bit physical pages')
    ck('sampled' not in json.dumps(X.TEXTURE_BACKING_MANIFEST).lower() or 'not bytes proven sampled' in json.dumps(X.TEXTURE_BACKING_MANIFEST), 'no sampled-bytes claim')


@test('T3B post-translation table wiring: with the recorded table both stages are translated, the requested union is the post-mask union, requested binding opportunities carry backing, the shader table holds the post bindings once, the constant table holds only referenced constants with backing only for requested ones, and the census conserves; a narrower post mask (t1 only) leaves t4 analysis-only with no backing; a wider one (t6) requests an unreferenced constant; VS/PS overlap')
def _():
    p = binding_trace('t3b_basic', vs_simple(), PS_A, CONSTS_A)
    tab = pm_table('t3b_basic', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    r = s['static_texture_bindings'][0]
    eq(list(r), ['draw_ord', 'frame', 'mode', 'vs', 'ps', 'analysis_status', 'stages', 'union_slots', 'overlap_slots', 'bindings', 'type2_slots',
                 'unused_type2_slots', 'referenced_non_texture_slots', 'post_translation', 'requested_binding_opportunities', 'resolve_relation'],
       '/6 row keys plus the /7 per-draw relation summary')
    eq(list(r['stages']['ps']), ['status', 'reason', 'ucode_hash', 'analysis_key', 'binding_mask'], '/6 stage keys: references + mask')
    pt = r['post_translation']
    eq(list(pt), ['vs', 'ps', 'status', 'requested_union', 'requested_overlap', 'requested_equals_analysis_union', 'requested_minus_analysis',
                  'analysis_minus_requested'], 'post block keys')
    eq((dict(pt['vs']), dict(pt['ps']), pt['status'], pt['requested_union'], pt['requested_overlap'], pt['requested_equals_analysis_union'],
        pt['requested_minus_analysis'], pt['analysis_minus_requested']),
       (dict(status='translated', mask=0), dict(status='translated', mask=0x12), 'complete', [1, 4], [], True, [], []), 'post block')
    eq([dict(b) for b in r['bindings']],
       [dict(slot=1, stages=['ps'], fetch_constant_key=fkey(TFC_1), is_texture_constant=True, requested_stages=['ps']),
        dict(slot=4, stages=['ps'], fetch_constant_key=fkey(TFC_4), is_texture_constant=True, requested_stages=['ps'])], 'bindings reference the constant table')
    eq([dict(o) for o in r['requested_binding_opportunities']],
       [dict(slot=1, stages=['ps'], fetch_constant_key=fkey(TFC_1), backing_status='complete', join_usable=True),
        dict(slot=4, stages=['ps'], fetch_constant_key=fkey(TFC_4), backing_status='complete', join_usable=True)], 'requested binding opportunities')
    sh = s['static_texture_shaders']
    ps_key = hashlib.sha256(be_bytes(PS_A)).hexdigest()
    eq(sorted(sh), sorted([hashlib.sha256(be_bytes(vs_simple())).hexdigest(), ps_key]), 'shader table keyed by ucode SHA-256')
    e = sh[ps_key]
    eq(list(e), ['analysis_key', 'ucode_sha256', 'ucode_xxh3_64', 'ucode_bytes', 'unmapped_bytes', 'stages_seen', 'analysis', 'post_translation'], 'shader entry keys')
    eq((e['analysis_key'], e['ucode_xxh3_64'], e['ucode_bytes'], e['unmapped_bytes'], e['stages_seen'], r['stages']['ps']['analysis_key'], r['stages']['ps']['ucode_hash']),
       (ps_key, '%016X' % eng.draws[0]['ps'], len(be_bytes(PS_A)), 0, ['ps'], ps_key, eng.draws[0]['ps']), 'identities: content key + trace hash')
    eq(e['analysis'], json.loads(json.dumps(X.analyze_texture_fetches(be_bytes(PS_A)))), 'the full analysis once')
    eq(e['post_translation'], {'ps': dict(status='translated', reason=None, mask=0x12, binding_slots=[1, 4],
                                          bindings=[dict(fetch_constant=1, dimension=1, dimension_name='k2D', is_signed=0),
                                                    dict(fetch_constant=4, dimension=2, dimension_name='k3DOrStacked', is_signed=0)])}, 'post bindings once, per stage')
    fc = s['texture_fetch_constants']
    eq(sorted(fc), sorted([fkey(TFC_1), fkey(TFC_4)]), 'constant table: referenced constants only (the unused slot 6 is not a definition)')
    c1 = fc[fkey(TFC_1)]
    eq(list(c1), ['key', 'raw', 'decoded', 'requested', 'backing', 'backing_reason'], 'constant entry keys')
    eq((c1['raw'], c1['decoded'], c1['requested'], c1['backing']['status'], c1['backing_reason']),
       (TFC_1, json.loads(json.dumps(X.decode_tfetch(TFC_1))), True, 'complete', None), 'constant entry')
    eq(c1['backing'], json.loads(json.dumps(X.derive_texture_backing(TFC_1))), 'backing == derive_texture_backing(raw)')
    c = s['requested_binding_census']
    t = c['post_translation_table']
    eq((t['supplied'], t['pin_matches'], t['shaders_in_table'], t['xenia_pin'], t['file_sha256'], t['translator_options']),
       (True, True, 2, X.XENIA_PIN, hashlib.sha256(open(tab, 'rb').read()).hexdigest(), 'test table'), 'table identity in the census')
    eq((c['normal_draws_total'], c['post_translation']['vs']['translated'], c['post_translation']['ps']['translated'], c['post_translation']['draws_post_complete'],
        c['post_translation']['draws_post_not_complete']), (1, 1, 1, 1, 0), 'post status census')
    eq(c['requested_slots'], dict(denominator='per-draw slot counts over the draws whose VS and PS are both translated', vs=0, ps=2, union=2, overlap=0), 'requested slots')
    eq(c['requested_vs_analysis'], dict(draws_equal=1, draws_differ=0, slots_requested_not_analysis=0, slots_analysis_not_requested=0), 'delta')
    eq((c['distinct_shaders_translated'], c['distinct_fetch_constants'], c['distinct_requested_constants']), (dict(vs=1, ps=1), 2, 2), 'distinct')
    eq((c['constant_backing_status'], c['constant_backing_conserved'], c['constants_join_usable']),
       (dict(complete=2, invalid_type=0, no_data=0, invalid_1d=0, unsupported_format=0), True, 2), 'constant statuses')
    eq((c['occurrence_backing_status']['complete'], c['occurrences_total'], c['occurrences_join_usable']), (2, 2, 2), 'occurrences')
    eq((c['draws_all_requested_join_usable'], c['draws_with_requested_not_usable'], c['draws_no_requested_slots'], c['draws_conserved']), (1, 0, 0, True), 'draw census')
    ck('POST-TRANSLATION' in c['meaning'] and 'neither proves' in c['meaning'], 'the census says what it is not')
    # narrower post mask: only t1 requested
    tab = pm_table('t3b_narrow', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A, post_mask=0x02)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    r = s['static_texture_bindings'][0]
    pt = r['post_translation']
    eq((pt['ps']['mask'], pt['requested_union'], pt['requested_equals_analysis_union'], pt['requested_minus_analysis'], pt['analysis_minus_requested']),
       (0x02, [1], False, [], [4]), 'requested is the POST mask, not the analysis mask')
    eq(([b['requested_stages'] for b in r['bindings']], [o['slot'] for o in r['requested_binding_opportunities']]), ([['ps'], []], [1]), 't4 analysis-only')
    c4 = s['texture_fetch_constants'][fkey(TFC_4)]
    eq((c4['requested'], c4['backing'], c4['backing_reason']), (False, None, 'not requested by any post-translation mask'), 'no backing for an unrequested constant')
    c = s['requested_binding_census']
    eq((c['requested_slots']['ps'], c['requested_vs_analysis'], c['distinct_requested_constants'], c['constant_backing_status']['complete'], c['occurrences_total']),
       (1, dict(draws_equal=0, draws_differ=1, slots_requested_not_analysis=0, slots_analysis_not_requested=1), 1, 1, 1), 'narrow census')
    # wider post mask: t6 requested though no instruction names it
    tab = pm_table('t3b_wide', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A, post_mask=0x52)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    r = s['static_texture_bindings'][0]
    pt = r['post_translation']
    eq((pt['requested_union'], pt['requested_minus_analysis'], pt['analysis_minus_requested'], r['union_slots'], [b['slot'] for b in r['bindings']],
        [(o['slot'], o['backing_status']) for o in r['requested_binding_opportunities']], r['unused_type2_slots']),
       ([1, 4, 6], [6], [], [1, 4], [1, 4], [(1, 'complete'), (4, 'complete'), (6, 'invalid_1d')], [6]),
       'wider: t6 is an opportunity, not a static binding; its tiled 1D constant is a named invalid_1d with no interval')
    eq((s['texture_fetch_constants'][fkey(TFC_6)]['backing']['join_usable'], [o['join_usable'] for o in r['requested_binding_opportunities']]), (False, [True, True, False]),
       'invalid_1d is not join-usable')
    eq(s['texture_fetch_constants'][fkey(TFC_6)]['requested'], True, 'TFC_6 requested')
    eq(s['requested_binding_census']['requested_vs_analysis']['slots_requested_not_analysis'], 1, 'delta counted')
    # overlap
    p = binding_trace('t3b_overlap', VS_OVERLAP, PS_A, CONSTS_A + [(7, TFC_7)])
    tab = pm_table('t3b_overlap', [pm_entry('vs', VS_OVERLAP), pm_entry('ps', PS_A)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    r = s['static_texture_bindings'][0]
    pt = r['post_translation']
    eq((pt['vs']['mask'], pt['ps']['mask'], pt['requested_union'], pt['requested_overlap'], [(o['slot'], o['stages']) for o in r['requested_binding_opportunities']]),
       (0x90, 0x12, [1, 4, 7], [4], [(1, ['ps']), (4, ['vs', 'ps']), (7, ['vs'])]), 'overlap')
    c = s['requested_binding_census']
    eq((c['requested_slots']['vs'], c['requested_slots']['ps'], c['requested_slots']['union'], c['requested_slots']['overlap']), (2, 2, 3, 1), 'overlap census')
    eq(s['static_texture_shaders'][hashlib.sha256(be_bytes(VS_OVERLAP)).hexdigest()]['post_translation']['vs']['binding_slots'], [4, 7], 'VS post bindings')


@test('T3B post-translation fails closed by name: no table -> no_table (no requested slots, no backing, census says unsupplied); wrong pin -> not_in_table; a shader absent from the table -> not_in_table for that stage only; translation_valid 0 -> translation_invalid; a differing Xenia analysis mask -> analysis_mismatch; unmapped ucode -> ucode_unmapped; no shader -> missing; --post-masks with a missing file is REFUSED')
def _():
    p = binding_trace('t3b_fc', vs_simple(), PS_A, CONSTS_A)
    s, eng = summary(p)
    r = s['static_texture_bindings'][0]
    pt = r['post_translation']
    eq((dict(pt['vs']), dict(pt['ps']), pt['status'], pt['requested_union'], pt['requested_equals_analysis_union'], pt['analysis_minus_requested'],
        r['requested_binding_opportunities'], [b['requested_stages'] for b in r['bindings']]),
       (dict(status='no_table', mask=None), dict(status='no_table', mask=None), 'incomplete', [], None, [], [], [[], []]), 'no table: nothing requested')
    eq([(e['requested'], e['backing']) for e in s['texture_fetch_constants'].values()], [(False, None)] * 2, 'no backing derived')
    sh = s['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]
    eq(sh['post_translation']['ps'], dict(status='no_table', reason='no post-translation mask table supplied (--post-masks)', mask=None, binding_slots=[], bindings=[]), 'shader entry')
    c = s['requested_binding_census']
    eq((c['post_translation_table']['supplied'], c['post_translation_table']['path'], c['post_translation']['ps']['no_table'], c['post_translation']['draws_post_complete'],
        c['post_translation']['draws_post_not_complete'], c['requested_slots']['union'], c['distinct_requested_constants'], c['distinct_fetch_constants'],
        c['draws_no_requested_slots'], c['draws_conserved'], c['constant_backing_conserved']), (False, None, 1, 0, 1, 0, 0, 2, 0, True, True), 'census')
    tab = pm_table('t3b_pin', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A)], pin='0' * 40)
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    pt = s['static_texture_bindings'][0]['post_translation']
    eq((pt['vs']['status'], pt['ps']['status'], pt['status'], s['requested_binding_census']['post_translation_table']['pin_matches']),
       ('not_in_table', 'not_in_table', 'incomplete', False), 'pin mismatch refused')
    ck('is not the tool pin' in s['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['post_translation']['ps']['reason'], 'pin reason')
    tab = pm_table('t3b_absent', [pm_entry('vs', vs_simple())])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    pt = s['static_texture_bindings'][0]['post_translation']
    eq((pt['vs']['status'], pt['ps']['status'], pt['status'], pt['requested_union'], s['static_texture_bindings'][0]['requested_binding_opportunities']),
       ('translated', 'not_in_table', 'incomplete', [], []), 'PS absent from the table')
    c = s['requested_binding_census']
    eq((c['post_translation']['vs']['translated'], c['post_translation']['ps']['not_in_table'], c['post_translation']['draws_post_complete'], c['distinct_requested_constants']),
       (1, 1, 0, 0), 'absent census')
    tab = pm_table('t3b_invalid', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A, valid=0)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    eq(s['static_texture_bindings'][0]['post_translation']['ps']['status'], 'translation_invalid', 'invalid translation')
    ck('1 errors' in s['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['post_translation']['ps']['reason'], 'errors named')
    tab = pm_table('t3b_mismatch', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A, xenia_mask=0x10)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    eq(s['static_texture_bindings'][0]['post_translation']['ps']['status'], 'analysis_mismatch', 'analysis mismatch')
    ck("00000010 differs from this tool's 00000012" in s['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['post_translation']['ps']['reason'], 'masks named')
    ps = PS_A
    tr = base_trace().packet(im_load_imm(0, vs_simple())).mem_read(0x1F800000, be_bytes(ps)[:8]).packet(im_load(1, 0x1F800000, len(ps)))
    for slot, dw in CONSTS_A:
        tr.packet(set_const(1, slot * 6, *dw))
    pu = tr.packet(draw2()).save('t3b_unmapped')
    tab = pm_table('t3b_unmapped', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A)])
    s, eng = summary(pu, post_table=X.load_post_mask_table(tab))
    r = s['static_texture_bindings'][0]
    eq((r['post_translation']['ps']['status'], r['post_translation']['status'], r['stages']['ps']['analysis_key'].endswith('-unmapped')), ('ucode_unmapped', 'incomplete', True), 'unmapped')
    e = s['static_texture_shaders'][r['stages']['ps']['analysis_key']]
    eq((e['ucode_xxh3_64'], e['unmapped_bytes'] > 0, e['post_translation']['ps']['status']), (None, True, 'ucode_unmapped'), 'unmapped entry: no trace identity invented')
    s, eng = summary(binding_trace('t3b_no_ps', vs_simple(), None, [(1, TFC_1)]), post_table=X.load_post_mask_table(tab))
    pt = s['static_texture_bindings'][0]['post_translation']
    eq((dict(pt['ps']), pt['vs']['status'], pt['status'], s['requested_binding_census']['post_translation']['ps']['missing']),
       (dict(status='missing', mask=None), 'translated', 'incomplete', 1), 'missing PS')
    rc, so, se = run_main([p, '--post-masks', os.path.join(TMP, 'no_such_table.json')])
    eq(rc, X.EXIT_USAGE, 'missing table refused: %r' % se)
    ck('REFUSED' in se and 'no such post-translation mask table' in se, se)


@test('T3B normalization is lossless and referential: content keys (64-hex SHA-256 analysis key, 48-hex exact raw constant key), one definition per distinct shader / constant across draws, the expander rebuilds the /5 rows by COPYING from the tables (a table edit shows in the expansion), derives binding slots from the mask and per-stage provenance counts from binding instructions only, is deterministic, and round-trips through JSON')
def _():
    p = binding_trace('t3b_norm', VS_OVERLAP, PS_A, CONSTS_A + [(7, TFC_7)], draws=3)
    tab = pm_table('t3b_norm', [pm_entry('vs', VS_OVERLAP), pm_entry('ps', PS_A)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    for k in s['static_texture_shaders']:
        ck(len(k) == 64 and all(ch in '0123456789abcdef' for ch in k), 'analysis key %r' % k)
    for k, e in s['texture_fetch_constants'].items():
        ck(len(k) == 48 and all(ch in '0123456789ABCDEF' for ch in k) and k == fkey(e['raw']), 'constant key %r' % k)
    eq((len(s['static_texture_shaders']), len(s['texture_fetch_constants']), len(s['static_texture_bindings'])), (2, 3, 3),
       'one definition per distinct shader / referenced constant (t1, t4, t7; the unreferenced t6 is none), three rows')
    eq(len({json.dumps(dict(r, draw_ord=0), sort_keys=True) for r in s['static_texture_bindings']}), 1, 'rows identical apart from the ordinal')
    j = json.loads(json.dumps(s))
    e1 = X.expand_static_bindings_v5(s)
    e2 = X.expand_static_bindings_v5(j)
    eq(json.loads(json.dumps(e1)), e2, 'deterministic and JSON-stable')
    rows = e2['static_texture_bindings']
    r = rows[0]
    eq(list(r), ['draw_ord', 'frame', 'mode', 'vs', 'ps', 'analysis_status', 'stages', 'union_slots', 'overlap_slots', 'bindings', 'type2_slots',
                 'unused_type2_slots', 'referenced_non_texture_slots'], '/5 row keys')
    eq(list(r['stages']['ps']), ['status', 'reason', 'ucode_hash', 'binding_slots', 'binding_mask', 'instructions', 'binding_instructions',
                                 'non_binding_instructions', 'unknown_opcode_instructions'], '/5 stage keys')
    eq((r['stages']['vs']['binding_slots'], r['stages']['ps']['binding_slots'], r['stages']['ps']['binding_instructions'], r['stages']['ps']['non_binding_instructions'],
        len(r['stages']['ps']['instructions']), len(r['stages']['vs']['instructions'])), ([4, 7], [1, 4], 2, 1, 3, 2),
       'stage fields rebuilt from the tables (the VS vfetch is not a texture instruction)')
    eq([(b['slot'], b['stages'], b['instructions_per_stage'], b['fetch_constant_type_name'], b['is_texture_constant'], b['raw']) for b in r['bindings']],
       [(1, ['ps'], dict(vs=0, ps=1), 'kTexture', True, TFC_1), (4, ['vs', 'ps'], dict(vs=1, ps=1), 'kTexture', True, TFC_4), (7, ['vs'], dict(vs=1, ps=0), 'kTexture', True, TFC_7)],
       'bindings rebuilt: provenance counts from binding instructions only (the setTexLOD on t1 is not counted)')
    eq(r['bindings'][0]['decoded'], j['texture_fetch_constants'][fkey(TFC_1)]['decoded'], 'decoded copied from the table')
    eq(list(e2['static_texture_shader_analyses']), sorted('%016X' % h for h in (eng.draws[0]['vs'], eng.draws[0]['ps'])), 'analyses keyed by the trace hash, sorted')
    eq(e2['static_texture_shader_analyses']['%016X' % eng.draws[0]['ps']], j['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['analysis'], 'analysis copied')
    eq(e2['static_texture_binding_census'], j['static_texture_binding_census'], 'census carried')
    # one content-keyed entry even when the same ucode is bound to both stages
    p2 = binding_trace('t3b_norm_both', PS_A, PS_A, CONSTS_A)
    s2, eng2 = summary(p2, post_table=X.load_post_mask_table(pm_table('t3b_norm_both', [pm_entry('vs', PS_A), pm_entry('ps', PS_A)])))
    e = s2['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]
    eq((len(s2['static_texture_shaders']), e['stages_seen'], sorted(e['post_translation']), s2['static_texture_bindings'][0]['post_translation']['requested_overlap']),
       (1, ['vs', 'ps'], ['ps', 'vs'], [1, 4]), 'one shader entry shared by both stages, both post statuses kept')
    j['texture_fetch_constants'][fkey(TFC_1)]['decoded']['swizzle'] = 12345
    j['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['analysis']['instructions'][0]['fetch_constant'] = 9
    e3 = X.expand_static_bindings_v5(j)
    eq((e3['static_texture_bindings'][2]['bindings'][0]['decoded']['swizzle'], e3['static_texture_bindings'][1]['stages']['ps']['instructions'][0]['fetch_constant']),
       (12345, 9), 'the expansion references the tables (an edit shows in every row)')


@test('T3B requested census conservation over four draws: complete + all usable, complete with a no-data request, post-incomplete (PS absent from the table), complete with no request; constant and occurrence statuses, join_usable counts and both conservation flags with their denominators')
def _():
    PS_B = ps_prog(tfi(const=1))      # absent from the table; references slot 1 (the no-data constant by then)
    PS_C = ps_prog()
    tr = base_trace().packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, PS_A))
    for slot, dw in CONSTS_A:
        tr.packet(set_const(1, slot * 6, *dw))
    tr.packet(draw2())
    tr.packet(set_const(1, 1 * 6, *TFC_NODATA)).packet(draw2())
    tr.packet(im_load_imm(1, PS_B)).packet(draw2())
    tr.packet(im_load_imm(1, PS_C)).packet(draw2())
    p = tr.save('t3b_census')
    tab = pm_table('t3b_census', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A), pm_entry('ps', PS_C)])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    rows = s['static_texture_bindings']
    eq([(r['post_translation']['status'], r['post_translation']['requested_union'], [(o['slot'], o['backing_status'], o['join_usable']) for o in r['requested_binding_opportunities']])
        for r in rows],
       [('complete', [1, 4], [(1, 'complete', True), (4, 'complete', True)]), ('complete', [1, 4], [(1, 'no_data', False), (4, 'complete', True)]),
        ('incomplete', [], []), ('complete', [], [])], 'four rows')
    fc = s['texture_fetch_constants']
    eq(sorted(fc), sorted([fkey(TFC_1), fkey(TFC_4), fkey(TFC_NODATA)]), 'three distinct constants')
    nd = fc[fkey(TFC_NODATA)]
    eq((nd['requested'], isinstance(nd['backing'], dict), rows[2]['union_slots'], rows[2]['bindings'][0]['fetch_constant_key']), (True, True, [1], fkey(TFC_NODATA)),
       'the no-data constant stays requested with its backing although draw 3 references it without requesting it (one shared definition)')
    eq(nd['backing']['status'], 'no_data', 'its backing status')
    c = s['requested_binding_census']
    eq((c['normal_draws_total'], c['post_translation']['draws_post_complete'], c['post_translation']['draws_post_not_complete'],
        c['post_translation']['vs']['translated'], c['post_translation']['ps']['translated'], c['post_translation']['ps']['not_in_table']), (4, 3, 1, 4, 3, 1), 'post census')
    eq((c['requested_slots']['ps'], c['requested_slots']['union'], c['distinct_shaders_translated'], c['distinct_fetch_constants'], c['distinct_requested_constants']),
       (4, 4, dict(vs=1, ps=2), 3, 3), 'requested slots over the three post-complete draws')
    eq((c['constant_backing_status'], c['constant_backing_conserved'], c['constants_join_usable']),
       (dict(complete=2, invalid_type=0, no_data=1, invalid_1d=0, unsupported_format=0), True, 2), 'constant statuses over distinct requested constants')
    eq((c['occurrence_backing_status'], c['occurrences_total'], c['occurrences_join_usable']),
       (dict(complete=3, invalid_type=0, no_data=1, invalid_1d=0, unsupported_format=0), 4, 3), 'occurrence statuses over per-draw opportunities')
    eq((c['draws_all_requested_join_usable'], c['draws_with_requested_not_usable'], c['draws_no_requested_slots'], c['draws_conserved']), (1, 1, 1, True),
       'draw classes conserve to the post-complete count')
    c5 = s['static_texture_binding_census']
    eq((c5['normal_draws_total'], c5['draws_with_static_binding'], c5['draws_with_no_static_binding'], c5['conservation_holds']), (4, 3, 1, True), 'the /5 census still conserves')


@test('T3B JSON/CLI/manifest wiring: the six /6 keys in the file, --expand-v5 writes exactly the expander output, the texture backing manifest (version, full pin, pinned cvar default, 12 stages of full-pin permalinks), --textures prints the post-translation line and the requested rows with base/mip intervals, and the summary prints the requested line')
def _():
    p = binding_trace('t3b_cli', vs_simple(), PS_A, CONSTS_A)
    tab = pm_table('t3b_cli', [pm_entry('vs', vs_simple()), pm_entry('ps', PS_A)])
    out, exp = os.path.join(TMP, 't3b.json'), os.path.join(TMP, 't3b_v5.json')
    rc, so, se = run_main([p, '--post-masks', tab, '--json', out, '--expand-v5', exp, '--draw', '1', '--textures'])
    eq(rc, 0, 'exit; stderr=%r' % se)
    j = json.load(open(out, encoding='utf-8'))
    for k in ('static_texture_shaders', 'texture_fetch_constants', 'static_texture_bindings', 'static_texture_binding_census', 'texture_backing_manifest',
              'requested_binding_census'):
        ck(k in j, 'key %s' % k)
    eq((len(j['static_texture_shaders']), len(j['texture_fetch_constants']), j['requested_binding_census']['post_translation_table']['file_sha256']),
       (2, 2, hashlib.sha256(open(tab, 'rb').read()).hexdigest()), 'tables and table identity in the file')
    eq(j['static_texture_shaders'][hashlib.sha256(be_bytes(PS_A)).hexdigest()]['post_translation']['ps']['binding_slots'], [1, 4], 'post bindings in the file')
    bk = j['texture_fetch_constants'][fkey(TFC_1)]['backing']
    ck(isinstance(bk, dict) and bk.get('status') == 'complete', 'backing in the file: %r' % (bk,))
    eq(bk['base_interval'], dict(start=0x11060000, length=262144, end_exclusive=0x110A0000), 'backing interval in the file')
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    for k in ('static_texture_shaders', 'texture_fetch_constants', 'static_texture_bindings', 'requested_binding_census'):
        eq(j[k], json.loads(json.dumps(s[k])), 'file == summary: %s' % k)
    eq(json.load(open(exp, encoding='utf-8')), json.loads(json.dumps(X.expand_static_bindings_v5(j))), '--expand-v5 == expander(file)')
    m = j['texture_backing_manifest']
    eq(m, json.loads(json.dumps(X.TEXTURE_BACKING_MANIFEST)), 'manifest in the file')
    eq((m['backing_version'], m['xenia_commit'], len(m['xenia_commit']), m['host_defaults_assumed'], list(m['stages'])),
       ('xtr_draws-texture-backing/1', X.XENIA_PIN, 40, dict(gpu_allow_invalid_fetch_constants=True),
        ['request_mask', 'binding_info_from_fetch_constant', 'subresources_from_fetch_constant', 'base_format', 'format_block_geometry', 'packed_mip_level',
         'packed_mip_offset', 'guest_texture_layout', 'tiled_upper_bounds', 'texture_key_and_guest_sizes', 'constants', 'math']), 'manifest fields')
    for name, links in m['stages'].items():
        for u in links:
            ck(u.startswith('https://github.com/xenia-canary/xenia-canary/blob/%s/src/xenia/' % X.XENIA_PIN) and '#L' in u, '%s permalink %r' % (name, u))
    ck('not bytes proven sampled' in m['output'] or 'sampled' not in m['output'], 'output wording')
    ck('post-translation (complete; vs=translated ps=translated) requested=[1, 4] overlap=[] equalsAnalysis=True reqNotAnalysis=[] analysisNotReq=[]' in so, 'post line: %r' % so[-3000:])
    ck("    requested t1  ['ps'] backing=complete base=[11060000,+262144) mips=[110A0000,+131072) usable=1" in so, 'requested row t1: %r' % so[-3000:])
    ck("    requested t4  ['ps'] backing=complete base=[1F900000,+" in so, 'requested row t4')
    ck("requested=1['ps'] raw=%s" % fkey(TFC_1) in so, 'binding line carries the requested flag and the exact raw key')
    ck("requested: postTable=yes postComplete=1/1 vs={'translated': 1} ps={'translated': 1} | slots vs=0 ps=2 union=2 overlap=0 eqAnalysis=1 differ=0 | constants distinct=2 requested=2 backing={'complete': 2} usable=2 | draws allUsable=1 notUsable=0 noRequested=0 conserved=1" in so,
       'summary line: %r' % so[:3000])
    rc, so, se = run_main([p, '--draw', '1', '--textures'])
    ck('post-translation (incomplete; vs=no_table ps=no_table) requested=[] overlap=[] equalsAnalysis=None' in so and '    ps post: no_table - no post-translation mask table supplied (--post-masks)' in so,
       'no table view: %r' % so[-3000:])
    ck('requested: postTable=NO postComplete=0/1' in so, 'no table summary line')


# ── TOOL-3C: temporal resolve/consumer interval relation (schema /7) ─────────

BASE_A_START, BASE_A_END, BASE_A_LEN = 0x1F130000, 0x1F2FD000, 1888256      # TRACE_CASES['base_A'] range (oracle vector)
C_EQUAL = tfc(fmt=6, dim=1, base=0x1F130000, w=1024, h=461, pitch=32, tiled=0)         # 4096*460 + 4*1024 = 1,888,256
C_PCC = tfc(fmt=2, dim=1, base=0x1F130000, w=64, h=64, pitch=2, tiled=0)               # 64*63 + 64 = 4096
C_CCP = tfc(fmt=6, dim=1, base=0x1F12F000, w=1024, h=1024, pitch=32, tiled=0)          # 4,194,304 from one page before
C_PART_R = tfc(fmt=6, dim=1, base=0x1F2F0000, w=1024, h=16, pitch=32, tiled=0)         # 65,536 across the end
C_ONE_BYTE = tfc(fmt=2, dim=1, base=0x1F12F000, w=1, h=5, pitch=32, tiled=0)           # 1024*4 + 1 = 4097: one byte inside
C_MIPS_IN = tfc(fmt=2, dim=1, base=0x1F000000, w=64, h=64, pitch=2, tiled=0, mipbase=0x1F130000, mipmax=3)  # mips 18,184 B inside
C_CONTACT_END = tfc(fmt=2, dim=1, base=0x1F2FD000, w=64, h=64, pitch=2, tiled=0)
C_CONTACT_START = tfc(fmt=2, dim=1, base=0x1F12F000, w=64, h=64, pitch=2, tiled=0)
C_PART_L = tfc(fmt=6, dim=1, base=0x1F12F000, w=1024, h=16, pitch=32, tiled=0)         # [1F12F000, 1F13F000)
C_TYPE0_IN = tfc(type_=0, fmt=2, dim=1, base=0x1F131000, w=64, h=64, pitch=2, tiled=0)
C_NEAR_3D = tfc(fmt=2, dim=1, base=0x1F131000, w=64, h=64, pitch=2, tiled=0)
PS_R1 = ps_prog(*[tfi(const=k) for k in range(1, 7)])
PS_R1B = ps_prog(*[tfi(const=k) for k in range(7, 10)])
PS_R2 = ps_prog(tfi(const=1), tfi(const=2), tfi(const=3))
PS_R3 = ps_prog(tfi(const=1), tfi(const=2))
PS_RB = ps_prog(tfi(const=1), tfi(const=4))
CONSTS_R1 = [(1, C_EQUAL), (2, C_PCC), (3, C_CCP), (4, C_PART_R), (5, C_ONE_BYTE), (6, C_MIPS_IN), (7, C_CONTACT_END),
             (8, C_CONTACT_START), (9, C_PART_L)]


def relation_trace(name, resolves, ps, consts, before=0, after=(None,)):
    """Normal draws (mode 4) BEFORE the kCopy resolves, the resolves (TRACE_CASES
    labels, mode 6, each with its own register state), then one normal draw
    per `after` entry (a program there is loaded as the PS first)."""
    tr = Trace().packet(t0(0x2208, 4)).packet(t0(0x2318, 1 << 20))
    for _n, (reg, val) in RANGE_REGS.items():
        tr.packet(t0(reg, val))
    tr.packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, ps))
    for slot, dw in consts:
        tr.packet(set_const(1, slot * 6, *dw))
    for _ in range(before):
        tr.packet(draw2())
    tr.packet(t0(0x2208, 6))
    for label in resolves:
        verts, endian, over = TRACE_CASES[label]
        for _n, (reg, val) in rr(**over).items():
            tr.packet(t0(reg, val))
        tr.packet(vf0(VF0_ADDR, 6, endian, 3))
        tr.packet_start(draw2())
        tr.mem_read(VF0_ADDR, swapped_payload(verts, endian))
        tr.packet_end()
    tr.packet(t0(0x2208, 4))
    for prog in after:
        if prog is not None:
            tr.packet(im_load_imm(1, prog))
        tr.packet(draw2())
    return tr.save(name)


def relation_summary(name, resolves, ps, consts, table_progs, **kw):
    p = relation_trace(name, resolves, ps, consts, **kw)
    tab = pm_table(name, [pm_entry('vs', vs_simple())] + [pm_entry('ps', prog) for prog in table_progs])
    s, eng = summary(p, post_table=X.load_post_mask_table(tab))
    return p, tab, s, eng


@test('T3C classifier against an exhaustive brute-force SET oracle: every pair of the 21 half-open intervals inside [0, 7) (441 pairs, all five relations present) agrees on the relation and on the exact intersection; hand-derived near-uint32 vectors (top-of-space containment, equality, endpoint contact, one-byte overlap, left/right partial); interval defects named (wrapped, inconsistent length, empty); the temporal rule is strict')
def _():
    ivs = [(a, b) for a in range(0, 6) for b in range(a + 1, 7)]
    seen = collections.Counter()
    for ps_, pe in ivs:
        for cs, ce in ivs:
            A, B = set(range(ps_, pe)), set(range(cs, ce))
            if not (A & B):
                exp = 'disjoint'
            elif A == B:
                exp = 'equal'
            elif B < A:
                exp = 'producer_contains_consumer'
            elif A < B:
                exp = 'consumer_contains_producer'
            else:
                exp = 'partial_overlap'
            rel, inter = X.classify_intervals(ps_, pe, cs, ce)
            eq(rel, exp, 'P[%d,%d) C[%d,%d)' % (ps_, pe, cs, ce))
            seen[rel] += 1
            if exp == 'disjoint':
                eq(inter, None, 'no intersection when disjoint')
            else:
                eq((inter['start'], inter['end_exclusive'], inter['length']), (min(A & B), max(A & B) + 1, len(A & B)),
                   'intersection P[%d,%d) C[%d,%d)' % (ps_, pe, cs, ce))
    eq(sum(seen.values()), 441, '21 x 21 pairs')
    ck(all(seen[k] > 0 for k in X.RELATIONS), 'every relation exercised: %s' % dict(seen))
    eq(X.classify_intervals(0xFFFFF000, 0x100000000, 0xFFFFF800, 0xFFFFFC00),
       ('producer_contains_consumer', dict(start=0xFFFFF800, end_exclusive=0xFFFFFC00, length=0x400)), 'containment at the top of the space')
    eq(X.classify_intervals(0xFFFFF000, 0x100000000, 0xFFFFF000, 0x100000000)[0], 'equal', 'equal at the top')
    eq(X.classify_intervals(0xFFFFFFFF, 0x100000000, 0xFFFFFFFE, 0xFFFFFFFF), ('disjoint', None), 'endpoint contact below the last byte')
    eq(X.classify_intervals(0x1000, 0x2000, 0x2000, 0x3000), ('disjoint', None), 'endpoint contact (producer end == consumer start)')
    eq(X.classify_intervals(0x1000, 0x2000, 0x0000, 0x1000), ('disjoint', None), 'endpoint contact (consumer end == producer start)')
    eq(X.classify_intervals(0x1000, 0x2000, 0x1FFF, 0x3000), ('partial_overlap', dict(start=0x1FFF, end_exclusive=0x2000, length=1)), 'one-byte overlap on the right')
    eq(X.classify_intervals(0x1000, 0x2000, 0x0000, 0x1001), ('partial_overlap', dict(start=0x1000, end_exclusive=0x1001, length=1)), 'one-byte overlap on the left')
    eq(X.classify_intervals(0, 1, 0, 1)[0], 'equal', 'one-byte equal')
    eq(X.classify_intervals(5, 6, 0, 10)[0], 'consumer_contains_producer', 'consumer contains')
    eq(X.classify_intervals(0, 10, 0, 5)[0], 'producer_contains_consumer', 'shared start: producer contains')
    eq(X.classify_intervals(0, 10, 5, 10)[0], 'producer_contains_consumer', 'shared end: producer contains')
    eq([X.interval_defect(*v) for v in ((10, 10, 0), (10, 5, 5), (10, 20, 5), (0xFFFFF000, 0x100000000, 0x1000), (0, 1, 1), (7, 3, 4294967292))],
       ['empty_interval', 'wrapped_interval', 'inconsistent_length', None, None, 'wrapped_interval'], 'interval defects (wrapped before length)')
    eq([X.temporally_eligible(p, c) for p, c in ((5, 6), (5, 5), (6, 5), (1, 100))], [True, False, False, True], 'strictly later only')
    eq((X.RELATIONS, X.NON_DISJOINT_RELATIONS, X.RELATION_VERSION),
       (('equal', 'producer_contains_consumer', 'consumer_contains_producer', 'partial_overlap', 'disjoint'),
        ('equal', 'producer_contains_consumer', 'consumer_contains_producer', 'partial_overlap'), 'xtr_draws-resolve-consumer-relation/1'), 'names')


@test('T3C trace fixture: one base_A resolve (draw 2) between a draw before it and two draws after it; the nine consumer constants give equal, producer-contains, consumer-contains, right/left partial, one-byte partial, both endpoint contacts and a base-disjoint/mips-inside pair; the earlier draw gets no pair; per-draw summaries, detail rows (with intersection, distance, observations only), census conservation, multiplicity, zero-overlap counts and the T3C2 sets')
def _():
    p, tab, s, eng = relation_summary('t3c_basic', ['base_A'], PS_R1, CONSTS_R1, [PS_R1, PS_R1B], before=1, after=(None, PS_R1B))
    rr_ = s['resolve_consumer_relation']
    eq(list(rr_), ['relation_version', 'meaning', 'definitions', 'canonical_cvar', 'producers', 'consumers', 'census', 't3c2', 'details', 'cvar_matrix'], 'keys')
    pr = rr_['producers']
    eq((pr['resolves_total'], pr['eligible'], pr['excluded_rows_total'], pr['excluded_rows'], len(pr['rows'])), (1, 1, 0, [], 1), 'one eligible producer')
    P = pr['rows'][0]
    eq((P['resolve_index'], P['draw_ord'], P['start'], P['end_exclusive'], P['length'], P['raw_base'], P['adjusted_base']),
       (0, 2, BASE_A_START, BASE_A_END, BASE_A_LEN, BASE_A_START, BASE_A_START), 'producer = the /4 range')
    eq(P['destination_observed'], dict(format=6, format_name='k_8_8_8_8', is_depth=False, bits_per_pixel=32, tiling_mode='2D', slice=0, pitch_aligned=1280,
                                       height_aligned=736, width_div_8=80, height_div_8=46, source_kind='color', source_format_name='k_8_8_8_8'), 'destination observations')
    rows = s['static_texture_bindings']
    eq([r['draw_ord'] for r in rows], [1, 3, 4], 'three normal draws')
    d1, d3, d4 = (r['resolve_relation'] for r in rows)
    eq((d1['post_status'], d1['eligible_earlier_producers'], d1['requested_occurrences'], d1['occurrences_eligible'], d1['components'], d1['candidate_pairs'],
        d1['relations'], d1['overlapping_producer_draw_ords']),
       ('complete', 0, 6, 6, dict(base=6, mips=1), 0, dict(equal=0, producer_contains_consumer=0, consumer_contains_producer=0, partial_overlap=0, disjoint=0), []),
       'draw 1 precedes the resolve: components exist, no candidate pair')
    eq((d3['eligible_earlier_producers'], d3['requested_occurrences'], d3['components'], d3['candidate_pairs'], d3['relations'], d3['non_disjoint_pairs'],
        d3['overlapping_producer_draw_ords'], d3['occurrences_excluded'], d3['components_excluded']),
       (1, 6, dict(base=6, mips=1), 7, dict(equal=1, producer_contains_consumer=2, consumer_contains_producer=1, partial_overlap=2, disjoint=1), 6, [2],
        dict(cvar_excluded=0, backing_not_join_usable=0), dict(absent=5, wrapped_interval=0, inconsistent_length=0, empty_interval=0)),
       'draw 3 (five constants define no mip interval: their mips component is absent, not invented)')
    eq((d4['components'], d4['candidate_pairs'], d4['relations'], d4['overlapping_producer_draw_ords']),
       (dict(base=3, mips=0), 3, dict(equal=0, producer_contains_consumer=0, consumer_contains_producer=0, partial_overlap=1, disjoint=2), [2]), 'draw 4: contacts disjoint, left partial')
    ce = rr_['census']
    eq((ce['candidate_pairs'], ce['relations'], ce['conservation_holds'], ce['non_disjoint_pairs'], ce['detail_rows'], ce['detail_rows_equal_non_disjoint']),
       (10, dict(equal=1, producer_contains_consumer=2, consumer_contains_producer=1, partial_overlap=3, disjoint=3), True, 7, 7, True), 'census conserves')
    eq((ce['producers_with_later_overlap'], ce['producers_with_zero_later_overlap'], ce['occurrences_with_zero_overlap'], ce['components_with_zero_overlap']),
       (1, dict(count=0, draw_ords=[]), 8, 10), 'zero-overlap counts (draw 1: 6 occurrences / 7 components; draw 4: 2 / 2; draw 3: 0 / 1)')
    eq(ce['multiplicity'], {'producers_per_component': {'0': 10, '1': 7}, 'components_per_producer': {'7': 1}, 'max_producers_per_component': 1,
                            'components_with_multiple_producers': 0}, 'multiplicity')
    eq((ce['t3c1_any_non_disjoint_pair'], ce['t3c3_every_component_at_most_one_producer']), (True, True), 'T3C1 / T3C3 on this fixture')
    cs = rr_['consumers']
    eq(cs, dict(normal_draws=3, draws_post_complete=3, draws_post_incomplete=0, requested_occurrences=15, occurrences_eligible=15,
                occurrences_excluded=dict(cvar_excluded=0, backing_not_join_usable=0), components=dict(base=15, mips=2, total=17),
                components_excluded=dict(absent=13, wrapped_interval=0, inconsistent_length=0, empty_interval=0)), 'consumer denominators')
    det = rr_['details']
    eq([(d['relation'], d['consumer']['draw_ord'], d['consumer']['slot'], d['consumer']['component'], d['intersection']['length'], d['ordinal_distance']) for d in det],
       [('equal', 3, 1, 'base', BASE_A_LEN, 1), ('producer_contains_consumer', 3, 2, 'base', 4096, 1), ('consumer_contains_producer', 3, 3, 'base', BASE_A_LEN, 1),
        ('partial_overlap', 3, 4, 'base', 0xD000, 1), ('partial_overlap', 3, 5, 'base', 1, 1), ('producer_contains_consumer', 3, 6, 'mips', 18184, 1),
        ('partial_overlap', 4, 9, 'base', 0xF000, 2)], 'detail rows in draw / slot / component order; only non-disjoint pairs')
    d0 = det[0]
    eq(list(d0), ['relation', 'producer', 'consumer', 'intersection', 'ordinal_distance'], 'detail keys (metadata referenced, not repeated)')
    eq(dict(d0['producer']), dict(resolve_index=0, draw_ord=2, start=BASE_A_START, end_exclusive=BASE_A_END, length=BASE_A_LEN, raw_base=BASE_A_START, adjusted_base=BASE_A_START), 'detail producer')
    eq(dict(d0['consumer']), dict(draw_ord=3, slot=1, stages=['ps'], fetch_constant_key=fkey(C_EQUAL), component='base', start=BASE_A_START, end_exclusive=BASE_A_END, length=BASE_A_LEN), 'detail consumer')
    eq(dict(d0['intersection']), dict(start=BASE_A_START, end_exclusive=BASE_A_END, length=BASE_A_LEN), 'detail intersection')
    ck_key = s['texture_fetch_constants'][d0['consumer']['fetch_constant_key']]['backing']['key']
    eq((ck_key['format_name'], ck_key['dimension_name'], ck_key['width_minus_1'] + 1, ck_key['height_minus_1'] + 1, ck_key['pitch_texels'], ck_key['tiled'], ck_key['packed_mips']),
       ('k_8_8_8_8', 'k2DOrStacked', 1024, 461, 1024, 0, 0), 'consumer key observations reachable through the referenced constant')
    eq(pr['rows'][d0['producer']['resolve_index']]['destination_observed']['format_name'], 'k_8_8_8_8', 'producer observations reachable through resolve_index')
    ck('observations' in rr_['definitions'] and 'repeats neither' in rr_['definitions']['observations'], 'the definitions say where the metadata lives')
    eq((det[4]['intersection'], det[5]['consumer']['start'], det[5]['consumer']['length'], det[6]['intersection']),
       (dict(start=BASE_A_START, end_exclusive=BASE_A_START + 1, length=1), BASE_A_START, 18184, dict(start=BASE_A_START, end_exclusive=0x1F13F000, length=0xF000)),
       'one-byte intersection; the mips component is its own interval; left partial intersection')
    ck(all(k not in json.dumps(det[0]).lower() for k in ('owner', 'compatible', 'executed', 'identity')), 'no owner / compatibility / execution words on a detail row')
    t2 = rr_['t3c2']
    eq(t2['denominators'], dict(candidate_pairs=10, non_disjoint_pairs=7), 'T3C2 denominators')
    eq((t2['raw_base_equal']['pairs'], t2['raw_base_equal']['and_non_disjoint'], t2['raw_base_equal']['false_positives_equal_but_disjoint']['count'],
        t2['raw_base_equal']['false_negatives_non_disjoint_not_equal']['count'], t2['raw_base_equal']['equals_full_relation']),
       (3, 3, 0, 4, False), 'raw-base equality misses containment/partial pairs that do not share the start')
    eq(t2['raw_base_equal']['false_negatives_non_disjoint_not_equal']['pairs'],
       ['resolve#0/draw#2>draw#3/t3/base/' + fkey(C_CCP), 'resolve#0/draw#2>draw#3/t4/base/' + fkey(C_PART_R), 'resolve#0/draw#2>draw#3/t5/base/' + fkey(C_ONE_BYTE),
        'resolve#0/draw#2>draw#4/t9/base/' + fkey(C_PART_L)], 'the false-negative pair keys')
    eq(t2['adjusted_start_equal'], t2['raw_base_equal'], 'adjusted == raw for this producer')
    mx = rr_['cvar_matrix']
    eq((mx['same_as_default'], mx['differences'], mx['true']['candidate_pairs'], mx['false']['candidate_pairs'], mx['true']['normalized_hash'] == mx['false']['normalized_hash']),
       (True, None, 10, 10, True), 'no type-0 constant: both cvar projections identical')
    rel = X.relation_for(eng, True)
    ck(X.relation_hash(rel) != X.relation_hash(dict(rel, details=[])), 'the normalized hash covers the detail rows')
    eq(X.relation_hash(rel), X.relation_hash(X.resolve_consumer_relation(eng, True)), 'hash is deterministic')


@test('T3C multiple earlier producers, exclusions and the cvar matrix: five resolves (base_A, msaa3, base_A, base_wrap, scissor_cut) leave three producers with every failed check named per excluded row; a later draw requests an inside constant (overlaps BOTH base_A resolves - both kept, none selected), a type-0 constant (included under the pinned default, excluded under cvar=false) and a no-data constant (backing not usable); a PS absent from the table makes the next draw post-incomplete; T3C3 refuted; T3C2 raw-base false positive from the scissor-cut producer')
def _():
    consts = [(1, C_PCC), (2, C_TYPE0_IN), (3, TFC_NODATA)]
    p, tab, s, eng = relation_summary('t3c_multi', ['base_A', 'msaa3', 'base_A', 'base_wrap', 'scissor_cut_600_300'], PS_R2, consts, [PS_R2],
                                      after=(None, PS_RB))
    rr_ = s['resolve_consumer_relation']
    pr = rr_['producers']
    eq((pr['resolves_total'], pr['eligible'], [r['draw_ord'] for r in pr['rows']], pr['excluded_rows_total']), (5, 3, [1, 3, 5], 2), 'producers')
    eq(pr['excluded_checks'], dict(not_join_usable=2, no_range=1, invariants_violated=1, wrapped_interval=1, inconsistent_length=0, empty_interval=0), 'every failed check counted')
    eq([(x['resolve_index'], x['draw_ord'], x['status'], x['reasons']) for x in pr['excluded_rows']],
       [(1, 2, 'rejected_msaa', ['not_join_usable', 'no_range']), (3, 4, 'complete', ['not_join_usable', 'invariants_violated', 'wrapped_interval'])], 'excluded rows named')
    eq((pr['rows'][2]['start'], pr['rows'][2]['end_exclusive'], pr['rows'][2]['raw_base'], pr['rows'][2]['adjusted_base']), (0x1F2AA000, BASE_A_END, BASE_A_START, 0x1F2AA000),
       'scissor-cut producer: raw base outside its own extent')
    rows = s['static_texture_bindings']
    eq([(r['draw_ord'], r['post_translation']['status']) for r in rows], [(6, 'complete'), (7, 'incomplete')], 'two normal draws')
    d6, d7 = rows[0]['resolve_relation'], rows[1]['resolve_relation']
    eq((d6['eligible_earlier_producers'], d6['requested_occurrences'], d6['occurrences_eligible'], d6['occurrences_excluded'], d6['components'], d6['candidate_pairs'],
        d6['relations'], d6['overlapping_producer_draw_ords']),
       (3, 3, 2, dict(cvar_excluded=0, backing_not_join_usable=1), dict(base=2, mips=0), 6,
        dict(equal=0, producer_contains_consumer=4, consumer_contains_producer=0, partial_overlap=0, disjoint=2), [1, 3]), 'draw 6: both base_A resolves kept')
    eq((d7['post_status'], d7['eligible_earlier_producers'], d7['requested_occurrences'], d7['candidate_pairs']), ('incomplete', 3, 0, 0), 'draw 7 post-incomplete: nothing compared')
    ce = rr_['census']
    eq((ce['candidate_pairs'], ce['relations'], ce['conservation_holds'], ce['detail_rows'], ce['producers_with_later_overlap'], ce['producers_with_zero_later_overlap'],
        ce['occurrences_with_zero_overlap'], ce['components_with_zero_overlap']),
       (6, dict(equal=0, producer_contains_consumer=4, consumer_contains_producer=0, partial_overlap=0, disjoint=2), True, 4, 2, dict(count=1, draw_ords=[5]), 0, 0), 'census')
    eq(ce['multiplicity'], {'producers_per_component': {'2': 2}, 'components_per_producer': {'0': 1, '2': 2}, 'max_producers_per_component': 2,
                            'components_with_multiple_producers': 2}, 'multiplicity: two producers per component')
    eq((ce['t3c1_any_non_disjoint_pair'], ce['t3c3_every_component_at_most_one_producer']), (True, False), 'T3C3 refuted on this fixture')
    eq(sorted((d['producer']['draw_ord'], d['consumer']['slot']) for d in rr_['details']), [(1, 1), (1, 2), (3, 1), (3, 2)], 'all four overlapping pairs materialized')
    eq(rr_['consumers'], dict(normal_draws=2, draws_post_complete=1, draws_post_incomplete=1, requested_occurrences=3, occurrences_eligible=2,
                              occurrences_excluded=dict(cvar_excluded=0, backing_not_join_usable=1), components=dict(base=2, mips=0, total=2),
                              components_excluded=dict(absent=2, wrapped_interval=0, inconsistent_length=0, empty_interval=0)), 'consumer denominators')
    t2 = rr_['t3c2']
    eq((t2['raw_base_equal']['pairs'], t2['raw_base_equal']['and_non_disjoint'], t2['raw_base_equal']['false_positives_equal_but_disjoint'],
        t2['raw_base_equal']['false_negatives_non_disjoint_not_equal']['count']),
       (3, 2, dict(count=1, pairs=['resolve#4/draw#5>draw#6/t1/base/' + fkey(C_PCC)]), 2), 'raw base equality: a false positive (scissor-cut raw base) and two false negatives')
    eq((t2['adjusted_start_equal']['pairs'], t2['adjusted_start_equal']['and_non_disjoint'], t2['adjusted_start_equal']['false_positives_equal_but_disjoint']['count'],
        t2['adjusted_start_equal']['false_negatives_non_disjoint_not_equal']['count']), (2, 2, 0, 2), 'adjusted start equality')
    mx = rr_['cvar_matrix']
    eq((mx['true']['occurrences_excluded_by_cvar'], mx['true']['occurrences_eligible'], mx['true']['components'], mx['true']['candidate_pairs'], mx['true']['relations'], mx['true']['detail_rows']),
       (0, 2, 2, 6, dict(equal=0, producer_contains_consumer=4, consumer_contains_producer=0, partial_overlap=0, disjoint=2), 4), 'matrix true')
    eq((mx['false']['occurrences_excluded_by_cvar'], mx['false']['occurrences_eligible'], mx['false']['components'], mx['false']['candidate_pairs'], mx['false']['relations'], mx['false']['detail_rows']),
       (1, 1, 1, 3, dict(equal=0, producer_contains_consumer=2, consumer_contains_producer=0, partial_overlap=0, disjoint=1), 2), 'matrix false: the type-0 occurrence filtered, nothing recomputed')
    eq((mx['same_as_default'], mx['differences']['changed_counts'], mx['differences']['changed_relations'], mx['differences']['affected_occurrences']),
       (False, dict(occurrences_eligible=[2, 1], components=[2, 1], candidate_pairs=[6, 3], detail_rows=[4, 2]),
        dict(producer_contains_consumer=[4, 2], disjoint=[2, 1]), [dict(draw_ord=6, slot=2, fetch_constant_key=fkey(C_TYPE0_IN))]), 'differences retained')
    ck(mx['true']['normalized_hash'] != mx['false']['normalized_hash'], 'hashes differ')
    eq(len(rr_['details']), 4, 'the canonical detail table is the default-true one')
    rf = X.relation_for(eng, False)
    eq((rf['allow_invalid_fetch_constants'], rf['consumers']['occurrences_excluded']['cvar_excluded'], [d['consumer']['slot'] for d in rf['details']]), (False, 1, [1, 1]), 'false relation')


@test('T3C raw vs adjusted base, temporal direction and direct defect injection: a 3D slice-3 resolve has an adjusted start past its raw base, so a consumer at the raw base is disjoint (raw-equal false positive) and one page later is a partial overlap; a range edited to invariants_hold=false / a wrong length / an empty extent is excluded by name while join_usable stays true; a consumer component edited to wrap is excluded by name; the relation is cached per cvar setting')
def _():
    p, tab, s, eng = relation_summary('t3c_3d', ['3d_slice3'], PS_R3, [(1, C_PCC), (2, C_NEAR_3D)], [PS_R3])
    rr_ = s['resolve_consumer_relation']
    P = rr_['producers']['rows'][0]
    eq((P['raw_base'], P['adjusted_base'], P['start'], P['end_exclusive'], P['destination_observed']['tiling_mode'], P['destination_observed']['slice']),
       (0x1F130000, 0x1F130000, 0x1F131400, 529047552, '3D', 3), '3D producer: the extent START is the slice-3 lower bound past the raw (= adjusted) base')
    ce = rr_['census']
    eq((ce['candidate_pairs'], ce['relations'], ce['detail_rows']), (2, dict(equal=0, producer_contains_consumer=0, consumer_contains_producer=0, partial_overlap=1, disjoint=1), 1), 'census')
    eq((rr_['details'][0]['consumer']['slot'], dict(rr_['details'][0]['intersection'])), (2, dict(start=0x1F131400, end_exclusive=0x1F132000, length=3072)), 'partial from the adjusted start')
    t2 = rr_['t3c2']
    eq((t2['raw_base_equal']['pairs'], t2['raw_base_equal']['and_non_disjoint'], t2['raw_base_equal']['false_positives_equal_but_disjoint']['pairs'],
        t2['raw_base_equal']['false_negatives_non_disjoint_not_equal']['count'], t2['adjusted_start_equal']['pairs'], t2['adjusted_start_equal']['false_negatives_non_disjoint_not_equal']['count']),
       (1, 0, ['resolve#0/draw#1>draw#2/t1/base/' + fkey(C_PCC)], 1, 0, 1), 'raw-base equality is neither necessary nor sufficient')
    g = eng.resolves[0]['guest_destination_range']
    rg0 = dict(g['range'])
    for edit, expect in ((dict(invariants_hold=False), ['invariants_violated']), (dict(extent_length=rg0['extent_length'] + 1), ['inconsistent_length']),
                         (dict(extent_end_exclusive=rg0['extent_start'], extent_length=0), ['empty_interval']),
                         (dict(extent_end_exclusive=rg0['extent_start'] - 1, extent_length=4294967295), ['wrapped_interval'])):
        g['range'] = dict(rg0, **edit)
        rel = X.resolve_consumer_relation(eng, True)
        eq((rel['producers']['eligible'], [x['reasons'] for x in rel['producers']['excluded_rows']], rel['census']['candidate_pairs']), (0, [expect], 0),
           'direct edit %s excluded by name with join_usable untouched' % edit)
    g['range'] = rg0
    b = eng.fetch_constants[fkey(C_NEAR_3D)]['backing']
    iv0 = dict(b['base_interval'])
    b['base_interval'] = dict(iv0, end_exclusive=iv0['start'] - 1)
    rel = X.resolve_consumer_relation(eng, True)
    eq((rel['consumers']['components_excluded'], rel['consumers']['components']['total'], rel['census']['candidate_pairs'], rel['census']['detail_rows']),
       (dict(absent=2, wrapped_interval=1, inconsistent_length=0, empty_interval=0), 1, 1, 0), 'a wrapped consumer component is excluded by name, never coerced')
    b['base_interval'] = iv0
    eq((X.relation_for(eng, True) is X.relation_for(eng, True), X.relation_for(eng, False) is X.relation_for(eng, True)), (True, False), 'cached per cvar setting')
    p2, tab2, s2, eng2 = relation_summary('t3c_before', ['base_A'], PS_R3, [(1, C_PCC), (2, C_NEAR_3D)], [PS_R3], before=2, after=())
    ce2 = s2['resolve_consumer_relation']['census']
    eq((ce2['candidate_pairs'], s2['resolve_consumer_relation']['producers']['eligible'], ce2['producers_with_zero_later_overlap'],
        [r['resolve_relation']['eligible_earlier_producers'] for r in s2['static_texture_bindings']]),
       (0, 1, dict(count=1, draw_ords=[3]), [0, 0]), 'consumers before the resolve are never compared; the resolve has zero LATER overlap')


@test('T3C JSON/CLI wiring: the /7 object in the file equals the summary (details, census, matrix), --relations lists producers, excluded rows and every non-disjoint pair, --draw N --textures prints the per-draw relation summary and its rows, the summary prints the relation line; the /5 expansion ignores the relation and stays lossless')
def _():
    p, tab, s, eng = relation_summary('t3c_cli', ['base_A'], PS_R1, CONSTS_R1, [PS_R1, PS_R1B], before=1, after=(None, PS_R1B))
    out = os.path.join(TMP, 't3c.json')
    rc, so, se = run_main([p, '--post-masks', tab, '--json', out, '--relations', '--draw', '3', '--textures'])
    eq(rc, 0, 'exit; stderr=%r' % se)
    j = json.load(open(out, encoding='utf-8'))
    ck('resolve_consumer_relation' in j and 'census' in j['resolve_consumer_relation'], 'relation object in the file')
    eq(j['resolve_consumer_relation'], json.loads(json.dumps(s['resolve_consumer_relation'])), 'file == summary')
    eq([r['resolve_relation'] for r in j['static_texture_bindings']], [json.loads(json.dumps(r['resolve_relation'])) for r in s['static_texture_bindings']], 'per-draw summaries in the file')
    eq((j['schema'], j['tool_version']), ('xtr_draws-summary/7', '2.6.0'), 'schema /7')
    v5 = X.expand_static_bindings_v5(j)
    eq((len(v5['static_texture_bindings']), 'resolve_relation' in v5['static_texture_bindings'][0]), (3, False), 'the /5 projection is unaffected')
    ck("resolve/consumer interval relation (xtr_draws-resolve-consumer-relation/1): producers 1/1 eligible, 10 candidate pairs, 7 non-disjoint (equal=1 pcc=2 ccp=1 partial=3); opportunities, not executed copies or samples; every overlapping earlier resolve listed, none selected" in so,
       'relations header: %r' % so[-4000:])
    ck('  producer resolve#0 draw#2 [1F130000,1F2FD000) +1888256 raw=1F130000 adj=1F130000 dest=k_8_8_8_8 2D' in so, 'producer line')
    ck("  resolve#0 draw#2 [1F130000,1F2FD000) +1888256 -> draw#3 t1 ['ps'] base [1F130000,1F2FD000) +1888256 : equal inter=[1F130000,1F2FD000) +1888256 dist=1" in so, 'equal row')
    ck("  resolve#0 draw#2 [1F130000,1F2FD000) +1888256 -> draw#3 t6 ['ps'] mips [1F130000,1F134708) +18184 : producer_contains_consumer inter=[1F130000,1F134708) +18184 dist=1" in so, 'mips row')
    ck("  resolve#0 draw#2 [1F130000,1F2FD000) +1888256 -> draw#4 t9 ['ps'] base [1F12F000,1F13F000) +65536 : partial_overlap inter=[1F130000,1F13F000) +61440 dist=2" in so, 'left partial row')
    eq(so.count(' : disjoint '), 0, 'no disjoint detail line')
    ck('  cvar matrix: true pairs=10 details=7 | false pairs=10 details=7 excludedByCvar=0 | same_as_default=True' in so, 'matrix line')
    ck("  resolve relation (complete): earlierProducers=1 occ=6/6 comps base=6 mips=1 pairs=7 {'equal': 1, 'producer_contains_consumer': 2, 'consumer_contains_producer': 1, 'partial_overlap': 2, 'disjoint': 1} overlappingResolveDraws=[2] (opportunities; no owner selected)" in so,
       'per-draw view: %r' % so[-4000:])
    ck("    resolve#0 draw#2 [1F130000,1F2FD000) +1888256 -> draw#3 t5 ['ps'] base [1F12F000,1F130001) +4097 : partial_overlap inter=[1F130000,1F130001) +1 dist=1" in so, 'one-byte row in the draw view')
    ck("relation: producers=1/1 excluded=0{} | consumers postComplete=3/3 occ=15/15 comps base=15 mips=2 excluded={} | pairs=10 equal=1 pcc=2 ccp=1 partial=3 disjoint=3 conserved=1 details=7 | resolvesNoLaterOverlap=0/1 maxProducersPerComponent=1 | T3C2 rawEq=False adjEq=False | cvarFalse same=1  relation=xtr_draws-resolve-consumer-relation/1" in so,
       'summary line: %r' % so[:4000])
    rc, so, se = run_main([p, '--relations'])
    ck('producers 1/1 eligible, 0 candidate pairs, 0 non-disjoint' in so and 'relation: producers=1/1' in so, 'no table: no consumers, no pairs: %r' % so[-1500:])
    p2, tab2, s2, eng2 = relation_summary('t3c_cli2', ['base_A', 'msaa3'], PS_R2, [(1, C_PCC), (2, C_TYPE0_IN), (3, TFC_NODATA)], [PS_R2])
    rc, so, se = run_main([p2, '--post-masks', tab2, '--relations'])
    ck("  excluded producer resolve#1 draw#2 status=rejected_msaa reasons=['not_join_usable', 'no_range']" in so, 'excluded producer line: %r' % so[-2000:])
    ck('  cvar matrix: true pairs=2 details=2 | false pairs=1 details=1 excludedByCvar=1 | same_as_default=False' in so, 'matrix differs line')


# ── iter-463 --coverage (D8: incomplete live traces must fail loudly) ─────────

COV_IB = 0x1F700000
TFC_CUBE = tfc(fmt=6, dim=3, base=0x11200000, w=64, h=64, d=6, pitch=2, mipmax=6, packed=1, mipbase=0x11300000)


def coverage_trace(name, supply_base, supply_mips, supply_ib):
    """VS vfetch slot 0 (supplied) + a PS tfetchCube of fetch slot 2 (a 64x64
    k_8_8_8_8 cube with a packed mip chain) + one DMA-indexed draw; the base,
    the mip region and the IB are supplied or not, each on its own."""
    b = X.derive_texture_backing(TFC_CUBE)
    bi, mi = b['base_interval'], b['mips_interval']
    tr = base_trace().packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, ps_prog(tfi(const=2, dim=3))))
    tr.packet(set_const(1, 2 * 6, *TFC_CUBE)).packet(vf0(VF0_ADDR))
    tr.packet_start(draw1(count=3, src=0, dma=(COV_IB, 3))).mem_read(VF0_ADDR, be_bytes(VERTS_A))
    if supply_base:
        tr.mem_read(bi['start'], b'\x11' * bi['length'])
    if supply_mips:
        tr.mem_read(mi['start'], b'\x22' * mi['length'])
    if supply_ib:
        tr.mem_read(COV_IB, b'\0\0\0\1\0\2')
    return tr.packet_end().save(name), bi, mi


@test('iter-463 --coverage: a draw whose cube base, cube mips or IB the trace never supplied is INCOMPLETE (exit 5) with the missing bytes per kind; all supplied is COMPLETE (exit 0); the cube mip region is counted (Xenia GetGuestTextureLayout, 6 faces)')
def _():
    p, bi, mi = coverage_trace('cov_none', False, False, False)
    ck(bi and mi and bi['length'] == 6 * 16384 and mi['length'] > 0, 'fixture: a 6-face base and a non-empty mip region (%r %r)' % (bi, mi))
    eng, _, _ = parse(p)
    rep = X.coverage_report(eng)
    c = rep['categories']
    eq((rep['complete'], rep['draws_incomplete'], c['tex_base']['missing'], c['tex_mips']['missing'], c['ib']['missing'], c['vb']['missing']),
       (False, [1], bi['length'], mi['length'], 6, 0), 'nothing supplied: base, mips and IB missing, VB supplied')
    rc, so, se = run_main([p, '--coverage'])
    eq(rc, X.EXIT_INCOMPLETE, 'exit; stderr=%r' % se)
    ck('coverage: INCOMPLETE' in so and 'kCube' in so, 'CLI names the incomplete cube:\n' + so)
    p, _, _ = coverage_trace('cov_base', True, False, True)
    rep = X.coverage_report(parse(p)[0])
    c = rep['categories']
    eq((rep['complete'], c['tex_base']['missing'], c['tex_mips']['missing'], c['ib']['missing']), (False, 0, mi['length'], 0),
       'base + IB supplied: only the cube mips are missing (the live-trace cube-mip class)')
    p, _, _ = coverage_trace('cov_all', True, True, True)
    rep = X.coverage_report(parse(p)[0])
    eq((rep['complete'], rep['draws_incomplete']), (True, []), 'everything supplied')
    rc, so, se = run_main([p, '--coverage'])
    eq(rc, X.EXIT_OK, 'exit; stderr=%r' % se)
    ck('coverage: COMPLETE' in so, 'CLI says COMPLETE:\n' + so)


@test('iter-463 --coverage is TEMPORAL (Xenia TracePlayer: a MemoryRead applies when reached, a draw runs at its PacketEnd): cube mips supplied only AFTER the draw packet (the old live traces\' swap-packet records) are LATE, the draw is incomplete (exit 5), and supplying them inside the draw packet is COMPLETE; a depth-only draw's PS-only slots are not needed (Xenia drops its PS)')
def _():
    b = X.derive_texture_backing(TFC_CUBE)
    bi, mi = b['base_interval'], b['mips_interval']

    def trace(name, mips_late, mode=4):
        tr = base_trace(mode).packet(im_load_imm(0, vs_simple())).packet(im_load_imm(1, ps_prog(tfi(const=2, dim=3))))
        tr.packet(set_const(1, 2 * 6, *TFC_CUBE)).packet(vf0(VF0_ADDR))
        tr.packet_start(draw1(count=3, src=0, dma=(COV_IB, 3))).mem_read(VF0_ADDR, be_bytes(VERTS_A))
        tr.mem_read(bi['start'], b'\x11' * bi['length']).mem_read(COV_IB, b'\0\0\0\1\0\2')
        if not mips_late:
            tr.mem_read(mi['start'], b'\x22' * mi['length'])
        tr.packet_end()
        if mips_late:
            tr.mem_read(mi['start'], b'\x22' * mi['length'])   # after the draw ran
        return tr.save(name)

    p = trace('cov_late', True)
    rep = X.coverage_report(parse(p)[0])
    c = rep['categories']
    eq((rep['complete'], rep['draws_incomplete'], c['tex_mips']['missing'], c['tex_mips']['late'],
        c['tex_mips']['late_draws'], c['tex_base']['late'], c['ib']['late']),
       (False, [1], 0, mi['length'], [1], 0, 0), 'mips supplied only after the draw: LATE, not MISSING, and incomplete')
    rc, so, se = run_main([p, '--coverage'])
    eq(rc, X.EXIT_INCOMPLETE, 'exit; stderr=%r' % se)
    ck('LATE=%d' % mi['length'] in so and 'lateDraws=1' in so and 'coverage: INCOMPLETE' in so, 'CLI reports LATE:\n' + so)
    p = trace('cov_intime', False)
    rep = X.coverage_report(parse(p)[0])
    eq((rep['complete'], rep['categories']['tex_mips']['late']), (True, 0), 'supplied inside the draw packet: complete')
    p = trace('cov_late_depth', True, mode=5)
    rep = X.coverage_report(parse(p)[0])
    eq((rep['complete'], rep['categories']['tex_mips']['late'], rep['ps_slots_unused']), (True, 0, 1),
       'a depth-only (kDepth) draw runs without its PS in Xenia (IsPixelShaderNeededWithRasterization): its PS-only cube is not needed')
    m = X.Memory()
    m.write(0x1000, b'\1' * 16)
    s1 = m.seq
    m.write(0x1008, b'\2' * 16)
    eq((m.missing_before(0x1000, 0x1018, s1), m.missing_before(0x1000, 0x1018, m.seq), m.missing_before(0x1000, 0x1018, 0),
        m.uncovered(0x1000, 0x1018)), (8, 0, 24, 0), 'first-supply ordinals: the overlapping later write only adds its new bytes')


# ── main ─────────────────────────────────────────────────────────────────────

def main(argv=None):
    global TMP
    keep = '--keep' in (argv or sys.argv[1:])
    TMP = tempfile.mkdtemp(prefix='xtr_draws_test_')
    total = passed = failed = exceptions = skipped = 0
    try:
        for name, fn in TESTS:
            total += 1
            try:
                fn()
            except Fail as e:
                failed += 1
                print('FAIL  %s\n      %s' % (name, e))
            except Skip as e:
                skipped += 1
                print('SKIP  %s (%s)' % (name, e))
            except Exception:  # noqa: BLE001 - reported as an exception, not a finding
                exceptions += 1
                print('EXC   %s\n%s' % (name, traceback.format_exc()))
            else:
                passed += 1
                print('ok    %s' % name)
    finally:
        if keep:
            print('fixtures kept in', TMP)
        else:
            shutil.rmtree(TMP, ignore_errors=True)
    print('module under test:', MOD_PATH)
    print('XTR_DRAWS TESTS: total=%d passed=%d failed=%d exceptions=%d skipped=%d'
          % (total, passed, failed, exceptions, skipped))
    return 0 if (failed == 0 and exceptions == 0) else 1


if __name__ == '__main__':
    sys.exit(main())
