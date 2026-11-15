"""Analyze the texture-path allocation/upload census (iter-458, Astra 068,
corrected per Astra 069 and 070).

SR_TEX_OWN_CENSUS=<file> makes the game record one totally ordered event stream
(kernel/tex_census.hpp) and write it ONCE when the live SRHT recorder first
closes. This tool checks the stream, joins it to the recorder's trace, then
evaluates every recorded draw-texture BINDING of the declared cohort on two
independent axes, against two BOUNDED hypotheses (Astra 070 re-registration):

  H-OWNER-OVERLAP (registered-owner overlap): the bound texture's physical span
    is ALIASED - two registered, not-observed-released grants with distinct
    serials overlapping EACH OTHER inside the span - at the resident upload's
    observation, or at the draw's publish.
  H-SAMPLED-STALE (sampled allocation/upload observation): the slot band the
    fetch constant declares holds an upload whose IMMUTABLE payload differs
    (3 x 64-byte probes) from the producer's latest observation, completed
    before the draw's publish, of the same descriptor in the upload's alias
    (STALE-CONTENT); or the registered owner set changed with equal sampled
    bytes (STALE-OWNER); or the band holds another descriptor of the base
    (RESIDENT-MISMATCH).

SCOPE: memory generations = grant serials, slot versions = apply ordinals,
descriptors = (base, fmtBits, dims); D3D9 texture-object generations are NOT
recorded; "live" = registered and not observed released. Zero sampled changes
kill only the sampled-observation claim - never unsampled reuse,
executed-fetch involvement or speckle causation. A SURVIVAL is an observation,
not a cause.

ORDER (Astra 070): every observation carries the FIFO-clock bracket [P0, P1];
P1 is read INSIDE the census's event mutex immediately before the append, so
the bracket covers the observation and its append, and observations of one
kind are serialized (append order == observation order). A render-side event's
z is its command's FIFO index E. An observation precedes draw E's publish iff
P1 < E, follows it iff P0 >= E, and is otherwise AMBIGUOUS -> UNKNOWN.
Selection follows the brackets, never the file order; an observation whose
P1 < E but which was appended after E's binding violates the contract
(C-zorder).

File layout (little-endian):
  header  u32 magic 'TXC1', u32 version 2, u32 recBytes 80, u32 routes,
          u64 events, u64 overflow, u64 lastSeq, u64 byType[9]
  events  {u32 type,a,b,c,d,e,f,g; u64 seq,x,y,z,w,v} x events

COMPLETENESS (each FAIL makes every verdict provisional):
  C-header   magic, version, record size; file size == header + events
  C-seq      seqs are exactly 1..events and lastSeq == events + overflow
  C-overflow no event was dropped
  C-bytype   per-type counts equal the header's
  C-inflight no bind observation was still in flight when the file was written
  C-lockstep apply k <-> publish k: same key/dims/fmt and the same FIFO index
  C-pairing  an ISSUED decode names a publish of ITS descriptor, inside its
             bracket, with the same payload hash
  C-resident every upload slot write is followed by its apply witness (same
             FIFO index, band, ordinal) and every applied upload wrote a slot;
             each binding's recorded per-band residents equal the reconstruction
  C-zorder   render-side z never decreases; publish z strictly increases;
             P0 <= P1; no publish recorded after a decode that read its index;
             no observation with P1 < E appended after draw E's binding
  C-grants   every release names a registered, unreleased grant
  C-owners   every decode's owners (count, kinds, serials, full-set digest,
             exact aliasing) equal the owners reconstructed from the chronology
  C-window   one open and one close edge; recorded in-window bindings ==
             bindings seen - white-out skips; every binding's in-window bit
             agrees with the edges' FIFO positions; draws at the tap <= draw
             commands; and, with --srht, the trace VALIDATES structurally, its
             live records == the FIFO commands in the window, its draw records
             == the draw commands executed there, and every in-window binding's
             FIFO index maps (record = openRecords + E - openZ - 2) to a DRAW
             record - an exact command-identity join

COHORT: the DECLARED cohort is every binding of a draw that reached the draw
tap while the recorder was open (conserved by C-window). Draws refused at
admission never reach the tap: they are OUTSIDE the cohort and are named. A
negative ("KILLED in the declared cohort") requires COMPLETE + WITNESSED (the
SRHT join included), a nonempty cohort, zero positives and ZERO exclusions
inside the cohort (UNKNOWN verdicts, non-upload residents, upload-time N/A,
white-out skips, no-opportunity bindings); otherwise it is a NARROW negative
with every exclusion named, or INCONCLUSIVE. Outside the window only flagged
pages record (first 64 per key): a biased sample, never a rate. A binding is
not a fetch: without --ps-sampler-map every verdict is "fetch unproven"; a
static map gives a fetch OPPORTUNITY, not execution. The band a binding reads
is the one its FETCH CONSTANT declares (dimension; stacked depth); the shader's
own fetch dimension is not verified.

The census's own tap time is reported; its effect on run timing is UNMEASURED.

Usage: python tools/tex_census.py <file> [--json out.json] [--ps-sampler-map m.json] [--srht trace.srht]
       python tools/tex_census.py --self-test
       python tools/tex_census.py --integration build/difftest.exe
"""
import bisect
import json
import os
import struct
import subprocess
import sys

MAGIC = 0x31435854
VERSION = 2
HDR_FMT = '<4I3Q9Q'
HDR = struct.calcsize(HDR_FMT)          # 112
EV_FMT = '<8I6Q'
EV = struct.calcsize(EV_FMT)            # 80
NTYPES = 9
GREG, GREL, DEC, PUB, APPLY, RES, DRAW, MARK = 1, 2, 3, 4, 5, 6, 7, 8
ISSUED, SUPPRESSED_CHANGED, NO_BASELINE = 1, 2, 3
FROM_UPLOAD, FROM_UNLOCK, FROM_RESOLVE = 1, 2, 3
APPLIED = 1
BAND_2D, BAND_3D, BAND_CUBE = 0, 1, 2
MARK_COST, MARK_OPEN, MARK_CLOSE = 1, 2, 3
DEC_UNSTABLE, DEC_ALIASED, DEC_UNREADABLE = 1 << 28, 1 << 29, 1 << 30
PHYS_MASK = 0x1FFFFFFF
PAGE_MASK = 0x1FFFF000
M64 = (1 << 64) - 1
# The executor's draw arm - DrawVertices, DrawIndexedVertices, BeginVertices,
# DrawVerticesUP (Astra 070: UP was missing from the denominator).
SRHT_DRAW_TYPES = (10, 11, 12, 21)
SRHT_PRESENT = 0
CHECKS = ('C-header', 'C-seq', 'C-overflow', 'C-bytype', 'C-inflight', 'C-lockstep', 'C-pairing',
          'C-resident', 'C-zorder', 'C-grants', 'C-owners', 'C-window')
H_OWN = 'H-OWNER-OVERLAP'
H_STALE = 'H-SAMPLED-STALE'


class Ev(object):
    __slots__ = ('type', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'seq', 'x', 'y', 'z', 'w', 'v')

    def __init__(self, t):
        (self.type, self.a, self.b, self.c, self.d, self.e, self.f, self.g,
         self.seq, self.x, self.y, self.z, self.w, self.v) = t

    def pack(self):
        return struct.pack(EV_FMT, self.type, self.a, self.b, self.c, self.d, self.e, self.f,
                           self.g, self.seq, self.x, self.y, self.z, self.w, self.v)


def parse(blob):
    """Returns (header dict, [Ev], [problems]). Never raises on bad input."""
    if len(blob) < HDR:
        return None, [], ['file shorter than the header (%d bytes)' % len(blob)]
    h = struct.unpack_from(HDR_FMT, blob, 0)
    hdr = {'magic': h[0], 'version': h[1], 'recBytes': h[2], 'routes': h[3],
           'events': h[4], 'overflow': h[5], 'lastSeq': h[6], 'byType': list(h[7:7 + NTYPES])}
    if hdr['magic'] != MAGIC or hdr['version'] != VERSION or hdr['recBytes'] != EV:
        return hdr, [], ['magic/version/recBytes %08X/%d/%d' %
                         (hdr['magic'], hdr['version'], hdr['recBytes'])]
    probs = []
    want = HDR + EV * hdr['events']
    if len(blob) != want:
        probs.append('size %d != header+events %d' % (len(blob), want))
    n = min(hdr['events'], (len(blob) - HDR) // EV)
    evs = [Ev(t) for t in struct.iter_unpack(EV_FMT, blob[HDR:HDR + EV * n])]
    return hdr, evs, probs


def write(hdr, evs):
    """Serialize (for the self-test)."""
    by = [0] * NTYPES
    for e in evs:
        if e.type < NTYPES:
            by[e.type] += 1
    h = dict(hdr)
    h.setdefault('byType', by)
    out = struct.pack(HDR_FMT, h.get('magic', MAGIC), h.get('version', VERSION),
                      h.get('recBytes', EV), h.get('routes', 1), h.get('events', len(evs)),
                      h.get('overflow', 0), h.get('lastSeq', len(evs) + h.get('overflow', 0)),
                      *h['byType'])
    return out + b''.join(e.pack() for e in evs)


def mix(serial):
    z = (serial * 0x9E3779B97F4A7C15) & M64
    return z ^ (z >> 31)


def aliased_hits(iv):
    """phys_window's exact sweep: intervals (lo, hi, serial) overlapping an
    earlier-starting interval of ANOTHER serial. >= 1 iff aliased."""
    hits, any_, best_hi, best_s, other_hi = 0, False, 0, 0, 0
    for lo, hi, s in sorted(iv, key=lambda t: (t[0], t[2])):
        reach = 0 if not any_ else (best_hi if s != best_s else other_hi)
        if any_ and lo < reach:
            hits += 1
        if not any_ or hi > best_hi:
            if any_ and s != best_s:
                other_hi = best_hi
            best_hi, best_s, any_ = hi, s, True
        elif s != best_s and hi > other_hi:
            other_hi = hi
    return hits


class Registry(object):
    """Mirrors phys_window's three grant registries and ownersOfSpan exactly."""

    def __init__(self):
        self.m = {1: {}, 2: {}, 3: {}}     # kind -> va -> [end, serial, state]
        self.keys = {1: [], 2: [], 3: []}  # sorted vas
        self.maxsz = {1: 0, 2: 0, 3: 0}
        self.lifetimes = {}                # serial -> dict

    def register(self, kind, va, size, serial, seq):
        m = self.m.setdefault(kind, {})
        self.keys.setdefault(kind, [])
        self.maxsz.setdefault(kind, 0)
        old = m.get(va)
        if old is not None:
            lt = self.lifetimes.get(old[1])
            if lt is not None and lt['end'] is None:
                lt['end'], lt['how'] = seq, 'replaced'
        else:
            bisect.insort(self.keys[kind], va)
        m[va] = [va + size, serial, 0]
        self.maxsz[kind] = max(self.maxsz[kind], size)
        self.lifetimes[serial] = {'kind': kind, 'va': va, 'size': size, 'reg': seq,
                                  'end': None, 'how': None}

    def release(self, kind, va, serial, how, seq):
        """True when the release names a registered grant still in the registry."""
        m = self.m.get(kind, {})
        ent = m.get(va)
        if ent is None or ent[1] != serial or (how == 1 and ent[2]):
            return False
        lt = self.lifetimes.get(serial)
        if how == 2:
            del m[va]
            i = bisect.bisect_left(self.keys[kind], va)
            if i < len(self.keys[kind]) and self.keys[kind][i] == va:
                del self.keys[kind][i]
            if lt is not None and lt['end'] is None:
                lt['end'], lt['how'] = seq, 'reclaimed'
        else:
            ent[2] = 1
            if lt is not None and lt['end'] is None:
                lt['end'], lt['how'] = seq, 'released'
        return True

    def owners(self, p, span):
        """ownersOfSpan: (nLive, kindBits, serial0, serial1, digest, aliasedHits)."""
        n, bits, s0, s1, dig = 0, 0, 0, 0, 0
        iv = []
        if not span:
            return n, bits, s0, s1, dig, 0
        lo = p & PHYS_MASK
        hi = min(lo + span, 0x20000000)
        for k in (1, 2, 3):
            m, keys, mx = self.m.get(k, {}), self.keys.get(k, []), self.maxsz.get(k, 0)
            for w in range(8):
                vlo, vhi = (w << 29) + lo, (w << 29) + hi
                i = bisect.bisect_left(keys, max(0, vlo - mx))
                while i < len(keys) and keys[i] < vhi:
                    va = keys[i]
                    end, serial, state = m[va]
                    i += 1
                    if end <= vlo or state:
                        continue
                    n += 1
                    bits |= 1 << k
                    if not s0 or serial < s0:
                        s1, s0 = s0, serial
                    elif not s1 or serial < s1:
                        s1 = serial
                    dig = (dig + mix(serial)) & M64
                    iv.append((max(va, vlo) - (w << 29), min(end, vhi) - (w << 29), serial))
        return n, bits, s0, s1, dig, aliased_hits(iv)

    def earlier_owner_ended(self, p, span, before_seq, exclude):
        lo = p & PHYS_MASK
        hi = lo + span
        for serial, lt in self.lifetimes.items():
            if serial in exclude or lt['end'] is None or lt['end'] > before_seq:
                continue
            glo = lt['va'] & PHYS_MASK
            if glo < hi and glo + lt['size'] > lo:
                return True
        return False


def overlaps(va, size, p, span):
    glo, lo = va & PHYS_MASK, p & PHYS_MASK
    return glo < lo + span and glo + size > lo


def srht_records(data):
    """(problems, [record types]) - the trace VALIDATED structurally first."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import srht_census as S
    try:
        _h, probs = S.validate(data, quiet=True)
    except Exception as ex:    # TraceError from the header (bad magic, short)
        return ['trace does not validate: %s' % ex], None
    if probs:
        return ['trace does not validate: %s' % '; '.join(probs[:3])], None
    return [], [cmd[0] for _i, cmd, _pb, _b in S.walk(data)]


def expected_band(dim, stacked):
    if dim == 3:
        return BAND_CUBE
    if dim == 2 or (dim == 1 and stacked):
        return BAND_3D
    return BAND_2D


def analyze(blob, ps_map=None, srht=None):
    hdr, evs, probs = parse(blob)
    fails = {k: [] for k in CHECKS}
    fails['C-header'].extend(probs)
    if hdr is None or not evs and probs:
        return {'header': hdr, 'checks': {k: 'FAIL' for k in CHECKS}, 'failures': fails,
                'complete': False, 'witness': {}, 'witnessed': False, 'draws': [], 'summary': {}}
    for i, e in enumerate(evs):
        if e.seq != i + 1:
            fails['C-seq'].append('event %d has seq %d (expected %d)' % (i, e.seq, i + 1))
            break
    if hdr['lastSeq'] != hdr['events'] + hdr['overflow']:
        fails['C-seq'].append('lastSeq %d != events %d + overflow %d' %
                              (hdr['lastSeq'], hdr['events'], hdr['overflow']))
    if hdr['overflow']:
        fails['C-overflow'].append('%d events dropped past the cap' % hdr['overflow'])
    by = [0] * NTYPES
    for e in evs:
        if e.type < NTYPES:
            by[e.type] += 1
    if by != hdr['byType']:
        fails['C-bytype'].append('counts %s != header %s' % (by, hdr['byType']))

    reg = Registry()
    grant_events = []                 # (P0, P1, kind, va, size, serial, op, seq)
    pubs, pubs_by_fifo, applies = {}, {}, {}
    issued_of_pub = {}                # publish ordinal -> ISSUED decode
    decodes = {}                      # (base, fmt16, dims) -> [Ev] in seq order
    all_decodes = []
    owner_bad = set()
    pending_res = {}                  # z -> Resident(upload) awaiting its apply
    resident = [dict(), dict(), dict()]
    draws = []
    marks = {MARK_OPEN: [], MARK_CLOSE: [], MARK_COST: []}
    last_render_z, last_pub_z, max_dec_p1 = 0, 0, 0
    for e in evs:
        t = e.type
        if t == GREG:
            reg.register(e.a, e.b, e.c, e.d, e.seq)
            grant_events.append((e.z, e.w, e.a, e.b, e.c, e.d, 'reg', e.seq))
            if e.z > e.w:
                fails['C-zorder'].append('seq %d: grant P0 %d > P1 %d' % (e.seq, e.z, e.w))
        elif t == GREL:
            lt = reg.lifetimes.get(e.d)
            size = lt['size'] if lt else 0
            if not reg.release(e.a, e.b, e.d, e.e, e.seq):
                fails['C-grants'].append('seq %d: release kind %d va %08X serial %d names no '
                                         'registered live grant' % (e.seq, e.a, e.b, e.d))
            grant_events.append((e.z, e.w, e.a, e.b, size, e.d, 'rel%d' % e.e, e.seq))
            if e.z > e.w:
                fails['C-zorder'].append('seq %d: grant P0 %d > P1 %d' % (e.seq, e.z, e.w))
        elif t == DEC:
            o = reg.owners(e.a, e.b)
            rec = ((e.e >> 8) & 0xFF, (e.e >> 16) & 0xFF, e.f, e.g, e.v, bool(e.e & DEC_ALIASED))
            want = (min(o[0], 255), o[1] & 0xFF, o[2], o[3], o[4], bool(o[5]))
            if rec != want:
                fails['C-owners'].append('seq %d base %08X: recorded %s, chronology %s' %
                                         (e.seq, e.a, rec, want))
                owner_bad.add(e.seq)
            if e.z > e.w:
                fails['C-zorder'].append('seq %d: decode P0 %d > P1 %d' % (e.seq, e.z, e.w))
            max_dec_p1 = max(max_dec_p1, e.w)
            decodes.setdefault((e.a, e.c & 0xFFFF, e.d), []).append(e)
            all_decodes.append(e)
            if (e.e & 0xFF) == ISSUED:
                pb = pubs_by_fifo.get(e.y)
                if (pb is None or pb.a != e.a or (pb.e & 0xFFFF) != (e.c & 0xFFFF) or pb.d != e.d
                        or pb.x != e.x or not (e.z < e.y <= e.w)):
                    fails['C-pairing'].append(
                        'seq %d: ISSUED decode of %08X names FIFO %d (bracket [%d,%d]) - %s' %
                        (e.seq, e.a, e.y, e.z, e.w,
                         'no such publish' if pb is None else 'publish differs'))
                else:
                    issued_of_pub[pb.f] = e
        elif t == PUB:
            if e.z <= last_pub_z:
                fails['C-zorder'].append('seq %d: publish z %d after %d' % (e.seq, e.z, last_pub_z))
            if e.z <= max_dec_p1:
                fails['C-zorder'].append('seq %d: publish z %d recorded after a decode read '
                                         'P1=%d' % (e.seq, e.z, max_dec_p1))
            last_pub_z = max(last_pub_z, e.z)
            if e.f in pubs:
                fails['C-lockstep'].append('seq %d: duplicate publish ordinal %d' % (e.seq, e.f))
            pubs[e.f] = e
            pubs_by_fifo[e.z] = e
        elif t in (APPLY, RES, DRAW):
            if e.z < last_render_z:
                fails['C-zorder'].append('seq %d: render-side z %d after %d' %
                                         (e.seq, e.z, last_render_z))
            last_render_z = max(last_render_z, e.z)
            if t == APPLY:
                if e.f in applies:
                    fails['C-lockstep'].append('seq %d: duplicate apply ordinal %d' % (e.seq, e.f))
                applies[e.f] = e
                pb = pubs.get(e.f)
                if pb is None:
                    fails['C-lockstep'].append('apply %d has no publish' % e.f)
                elif (pb.a, pb.d, pb.e, pb.z) != (e.a, e.d, e.e, e.z):
                    fails['C-lockstep'].append(
                        'apply %d: key/dims/fmt/z %08X/%08X/%X/%d != publish %08X/%08X/%X/%d' %
                        (e.f, e.a, e.d, e.e, e.z, pb.a, pb.d, pb.e, pb.z))
                r = pending_res.pop(e.z, None)
                if e.b == APPLIED:
                    if r is None or r.a != e.a or r.c != e.c or r.f != e.f:
                        fails['C-resident'].append('apply %d (applied, band %d) has no matching '
                                                   'upload slot write' % (e.f, e.c))
                elif r is not None:
                    fails['C-resident'].append('apply %d not applied but its command wrote a '
                                               'slot' % e.f)
            elif t == RES:
                if e.c > BAND_CUBE:
                    fails['C-resident'].append('seq %d: band %d' % (e.seq, e.c))
                else:
                    resident[e.c][e.a] = (e.b, e.f)
                if e.b == FROM_UPLOAD:
                    pending_res[e.z] = e
            else:
                rec = []
                for band, src_shift, ordv in ((BAND_2D, 16, e.g), (BAND_3D, 20, e.y & 0xFFFFFFFF),
                                              (BAND_CUBE, 24, e.y >> 32)):
                    pop = (e.e >> (12 + band)) & 1
                    rec.append((pop, ((e.e >> src_shift) & 0xF) if pop else 0, ordv if pop else 0))
                want = []
                for band in (BAND_2D, BAND_3D, BAND_CUBE):
                    rv = resident[band].get(e.c)
                    want.append((1, rv[0], rv[1]) if rv else (0, 0, 0))
                if rec != want:
                    fails['C-resident'].append('seq %d: binding of %08X records residents %s, the '
                                               'slot writes say %s' % (e.seq, e.c, rec, want))
                draws.append(e)
        elif t == MARK:
            if e.a in marks:
                marks[e.a].append(e)
            if e.a in (MARK_OPEN, MARK_CLOSE):
                if e.z < last_render_z:
                    fails['C-zorder'].append('seq %d: window edge z %d after %d' %
                                             (e.seq, e.z, last_render_z))
                last_render_z = max(last_render_z, e.z)
    for z, r in pending_res.items():
        fails['C-resident'].append('upload slot write at FIFO %d has no apply witness' % z)
    if applies and sorted(applies) != list(range(1, len(applies) + 1)):
        fails['C-lockstep'].append('apply ordinals are not 1..%d' % len(applies))
    if pubs and sorted(pubs) != list(range(1, len(pubs) + 1)):
        fails['C-lockstep'].append('publish ordinals are not 1..%d' % len(pubs))
    pending_at_close = sorted(o for o in pubs if o not in applies)
    # Observation-order contract (Astra 070). Decodes are serialized (g_obs) and
    # grant events by the registry lock, each reading P1 at its append, so P1 is
    # non-decreasing in file order - CHECKED here, then used: the first
    # observation appended after a binding must not claim P1 < E (appended
    # before E's publish, hence before its binding).
    obs_streams = (('decode', [(dq.seq, dq.z, dq.w) for dq in all_decodes]),
                   ('grant event', [(g[7], g[0], g[1]) for g in grant_events]))
    for kind, st in obs_streams:
        for i in range(1, len(st)):
            if st[i][2] < st[i - 1][2] or st[i][1] < st[i - 1][1]:
                fails['C-zorder'].append('seq %d: %s bracket [%d,%d] goes backwards after [%d,%d] '
                                         '(observations are serialized)' %
                                         (st[i][0], kind, st[i][1], st[i][2], st[i - 1][1],
                                          st[i - 1][2]))
                break
        seqs = [t[0] for t in st]
        for d in draws:
            j = bisect.bisect_right(seqs, d.seq)
            if j < len(st) and st[j][2] < d.z:
                fails['C-zorder'].append('seq %d: a %s with P1=%d < E=%d was appended after the '
                                         'binding (seq %d)' % (st[j][0], kind, st[j][2], d.z, d.seq))
                break
    cost = marks[MARK_COST][-1] if marks[MARK_COST] else None
    if cost is None:
        fails['C-inflight'].append('no census cost/in-flight Mark: the write is not attested')
    elif cost.c:
        fails['C-inflight'].append('%d bind observation(s) still in flight when written (waited '
                                   '%d ms%s)' % (cost.c, cost.d, ', timed out' if cost.e else ''))

    # C-window (+ the SRHT join)
    opens, closes = marks[MARK_OPEN], marks[MARK_CLOSE]
    open_m = opens[0] if len(opens) == 1 else None
    close = closes[0] if len(closes) == 1 else None
    if len(opens) != 1 or len(closes) != 1:
        fails['C-window'].append('%d open and %d close edges (need exactly one each)' %
                                 (len(opens), len(closes)))
    win_draws = [d for d in draws if (d.e >> 11) & 1]
    srht_joined = False
    if close is not None and open_m is not None:
        open_z = open_m.z
        rec_open = open_m.b | (open_m.c << 32)
        rec_close = close.b | (close.c << 32)
        if len(win_draws) != close.w - close.v:
            fails['C-window'].append('recorded in-window bindings %d != bindings seen %d - '
                                     'white-out skips %d' % (len(win_draws), close.w, close.v))
        if close.y > close.x:
            fails['C-window'].append('draws at the tap %d > draw commands %d' % (close.y, close.x))
        for d in draws:
            inside = open_z < d.z <= close.z
            if bool((d.e >> 11) & 1) != inside:
                fails['C-window'].append('seq %d: in-window bit %d but FIFO %d vs edges (%d, %d]' %
                                         (d.seq, (d.e >> 11) & 1, d.z, open_z, close.z))
                break
        if rec_close - rec_open != close.z - open_z:
            fails['C-window'].append('live records %d != FIFO commands in the window %d' %
                                     (rec_close - rec_open, close.z - open_z))
        if srht is not None:
            sprobs, types = srht_records(srht)
            if sprobs:
                fails['C-window'].extend(sprobs)
            else:
                ok = True
                if len(types) != rec_close:
                    fails['C-window'].append('trace has %d records, the close edge says %d' %
                                             (len(types), rec_close))
                    ok = False
                ndraw = sum(1 for tt in types[rec_open:rec_close] if tt in SRHT_DRAW_TYPES)
                if ndraw != close.x:
                    fails['C-window'].append('trace draw records %d != draw commands executed in '
                                             'the window %d' % (ndraw, close.x))
                    ok = False
                for d in win_draws:
                    idx = rec_open + d.z - open_z - 2
                    if not (rec_open <= idx < rec_close) or types[idx] not in SRHT_DRAW_TYPES:
                        fails['C-window'].append('binding seq %d (FIFO %d) maps to record %d, which '
                                                 'is not a draw' % (d.seq, d.z, idx))
                        ok = False
                        break
                srht_joined = ok
    checks = {k: ('FAIL' if fails[k] else 'PASS') for k in CHECKS}
    complete = all(v == 'PASS' for v in checks.values())
    witness = {
        'W-decode': by[DEC] > 0,
        'W-issued': bool(issued_of_pub),
        'W-publish': by[PUB] > 0,
        'W-apply': by[APPLY] > 0,
        'W-resident': by[RES] > 0,
        'W-draw-window': bool(win_draws),
        'W-grants': by[GREG] > 0,
        'W-window': bool(opens) and bool(closes),
        'W-srht-join': srht_joined,
    }
    witnessed = all(witness.values())

    # ── the per-binding axes ─────────────────────────────────────────────────
    ps_used = None
    if ps_map is not None:
        ps_used = {int(k, 16) if isinstance(k, str) else int(k): set(v) for k, v in ps_map.items()}
    reg2 = Registry()
    ordered_grants = sorted(grant_events, key=lambda g: (g[1], g[7]))   # by P1, then append
    gi = [0]

    def advance(E):
        while gi[0] < len(ordered_grants) and ordered_grants[gi[0]][1] < E:
            p0, p1, kind, va, size, serial, op, seq = ordered_grants[gi[0]]
            if op == 'reg':
                reg2.register(kind, va, size, serial, seq)
            else:
                reg2.release(kind, va, serial, 1 if op == 'rel1' else 2, seq)
            gi[0] += 1

    def ambiguous_grant(E, p, span):
        # after advance(E), the grants not applied have P1 >= E; P0 is also
        # non-decreasing, so the straddling ones are a contiguous run
        j = gi[0]
        while j < len(ordered_grants) and ordered_grants[j][0] < E:
            p0, p1, kind, va, size, serial, op, seq = ordered_grants[j]
            if overlaps(va, size, p, span):
                return True
            j += 1
        return False

    by_kfd = {}
    for (base, f16, dd), lst in decodes.items():
        by_kfd.setdefault((base, f16 & 0x3F, dd), []).extend(lst)
    for lst in by_kfd.values():
        lst.sort(key=lambda dq: dq.seq)

    def span_of(key, fmt6, dims, E):
        lst = by_kfd.get((key, fmt6, dims), [])
        i = bisect.bisect_left([dq.w for dq in lst], E) - 1
        return lst[i].b if i >= 0 else None

    reuse_memo = {}
    w_index = {}
    out = []
    for d in sorted(draws, key=lambda x: (x.z, x.seq)):
        key, fmt6, valid = d.c, d.e & 0xFF, (d.e >> 8) & 1
        dim, window, populated = (d.e >> 9) & 3, (d.e >> 11) & 1, (d.e >> 12) & 7
        stacked, dims, E = (d.e >> 15) & 1, d.f, d.z
        srcs = {BAND_2D: (d.e >> 16) & 0xF, BAND_3D: (d.e >> 20) & 0xF, BAND_CUBE: (d.e >> 24) & 0xF}
        ords = {BAND_2D: d.g, BAND_3D: d.y & 0xFFFFFFFF, BAND_CUBE: d.y >> 32}
        row = {'seq': d.seq, 'draw': d.a, 'sampler': d.b, 'key': key, 'window': window, 'E': E,
               'ps': d.x, 'notes': []}
        fetch = 'unproven'
        if ps_used is not None:
            fetch = ('opportunity' if d.b in ps_used.get(d.x, set()) else
                     'no-opportunity' if d.x in ps_used else 'unproven')
        row['fetch'] = fetch
        base_ok = (d.d & PAGE_MASK) == (key & PAGE_MASK)
        # CONTENT
        content, d0 = None, None
        want_band = expected_band(dim, stacked)
        if not valid:
            content = 'UNKNOWN(fetch-invalid)'
        elif not base_ok:
            content = 'UNKNOWN(fetch-base-mismatch)'
        elif populated == 0:
            content = 'UNKNOWN(no-resident)'
        elif bin(populated).count('1') > 1:
            content = 'UNKNOWN(multi-band)'
        elif populated != 1 << want_band:
            content = 'UNKNOWN(band-mismatch)'
        else:
            band = want_band
            src, ordn = srcs[band], ords[band]
            row['band'] = band
            if src == FROM_UNLOCK:
                content = 'NONUPLOAD(unlock)'
            elif src == FROM_RESOLVE:
                content = 'NONUPLOAD(resolve)'
            elif src != FROM_UPLOAD:
                content = 'UNKNOWN(resident-source-%d)' % src
            else:
                ap, pb = applies.get(ordn), pubs.get(ordn)
                if ap is None or pb is None:
                    content = 'UNKNOWN(resident-version-missing)'
                elif ap.a != key:
                    content = 'UNKNOWN(resident-key-mismatch)'
                elif (ap.e & 0x3F) != fmt6 or ap.d != dims:
                    content = 'RESIDENT-MISMATCH'
                    row['resident'] = {'fmt': ap.e & 0x3F, 'dims': ap.d, 'apply': ordn}
                d0 = issued_of_pub.get(ordn) if ap is not None else None
                if content is None:
                    if d0 is None:
                        content = 'UNKNOWN(no-issued-decode)'
                    else:
                        # BY BRACKET (Astra 070), never by file order: the latest
                        # observation that completed before E's publish; any that
                        # straddles it makes the verdict UNKNOWN.
                        lst = decodes[(d0.a, d0.c & 0xFFFF, d0.d)]
                        ws = w_index.setdefault(id(lst), [dq.w for dq in lst])
                        i = bisect.bisect_left(ws, E)         # P1 < E: the prefix
                        ld = lst[i - 1] if i > 0 and lst[i - 1].seq >= d0.seq else None
                        amb = i < len(lst) and lst[i].z < E and lst[i] is not d0
                        if ld is None:
                            content = 'UNKNOWN(upload-observed-after-publish)'
                        elif amb:
                            content = 'UNKNOWN(order-ambiguous)'
                        elif ld.seq in owner_bad or d0.seq in owner_bad:
                            content = 'UNKNOWN(owner-chronology-mismatch)'
                        elif ld is d0:
                            content = 'FRESH-SAMPLED' if d0.x else 'UNKNOWN(content-unreadable)'
                        elif not (ld.x and d0.x):
                            content = 'UNKNOWN(content-unreadable)'
                        elif ld.x != d0.x:
                            content = 'STALE-CONTENT'
                        elif ld.v != d0.v:
                            content = 'STALE-OWNER'
                        else:
                            content = 'FRESH-SAMPLED'
                        row['upload'] = {'apply': ordn, 'decodeSeq': d0.seq,
                                         'latestSeq': ld.seq if ld else None}
        row['content'] = content
        # OWNERSHIP at the resident upload's observation
        if not base_ok:
            up = 'UNKNOWN(fetch-base-mismatch)'
        elif d0 is None:
            up = 'N/A(no-upload-observation)' if content.startswith('NONUPLOAD') \
                else 'UNKNOWN(no-issued-decode)'
        elif d0.e & DEC_UNSTABLE:
            up = 'UNKNOWN(owners-unstable)'
        elif d0.seq in owner_bad:
            up = 'UNKNOWN(owner-chronology-mismatch)'
        else:
            up = 'ALIASED' if d0.e & DEC_ALIASED else 'CLEAN'
        row['ownUpload'] = up
        # OWNERSHIP at the draw's publish
        span = d0.b if d0 is not None else span_of(key, fmt6, dims, E)
        if not base_ok:
            dr = 'UNKNOWN(fetch-base-mismatch)'
        elif span is None:
            dr = 'UNKNOWN(span-unknown)'
        else:
            advance(E)
            if ambiguous_grant(E, key, span):
                dr = 'UNKNOWN(order-ambiguous)'
            else:
                o = reg2.owners(key, span)
                dr = 'ALIASED' if o[5] else 'CLEAN'
                if o[0] > 1 and not o[5]:
                    row['notes'].append('multi-cover')
                if o[0] >= 1:
                    live = set()
                    for k in (1, 2, 3):
                        for va, (end, serial, state) in reg2.m.get(k, {}).items():
                            if not state and overlaps(va, end - va, key, span):
                                live.add(serial)
                    first = min((reg2.lifetimes[s]['reg'] for s in live), default=d.seq)
                    mk = (key, span, first)
                    if mk not in reuse_memo:
                        reuse_memo[mk] = reg2.earlier_owner_ended(key, span, first, live)
                    if reuse_memo[mk]:
                        row['notes'].append('reuse')
        row['ownDraw'] = dr
        out.append(row)

    def tally(rows, field):
        c = {}
        for r in rows:
            c[r[field]] = c.get(r[field], 0) + 1
        return dict(sorted(c.items(), key=lambda kv: -kv[1]))

    cohort = [r for r in out if r['window']]
    flg = [r for r in out if not r['window']]
    no_opp = sum(1 for r in cohort if r['fetch'] == 'no-opportunity')
    whiteout = close.v if close is not None else 0
    refused = (close.x - close.y) if close is not None else 0
    elig = [r for r in cohort if r['fetch'] != 'no-opportunity']

    def verdict(pos, excl, decided):
        outside = '; outside the cohort: %d draw(s) refused at admission' % refused
        if not gate:
            return 'INCONCLUSIVE (not COMPLETE + WITNESSED incl. the SRHT join)'
        if pos:
            return ('SURVIVES (%d positive binding(s); an observation, not a cause)%s' %
                    (pos, outside))
        n_excl = sum(excl.values())
        if decided == 0:
            return 'INCONCLUSIVE (no decided binding in the declared cohort)%s' % outside
        if n_excl == 0:
            return ('KILLED in the declared cohort (0 positive over all %d bindings of draws '
                    'reaching the tap in the recorder window)%s' % (decided, outside))
        return ('NARROW negative only: 0 positive over %d decided binding(s); excluded within '
                'the cohort: %s%s' % (decided, ', '.join('%d %s' % (v, k) for k, v in excl.items()
                                                         if v), outside))

    gate = complete and witnessed
    own_pos = sum(1 for r in elig if 'ALIASED' in (r['ownDraw'], r['ownUpload']))
    own_excl = {
        'UNKNOWN': sum(1 for r in elig if 'ALIASED' not in (r['ownDraw'], r['ownUpload']) and
                       (r['ownDraw'].startswith('UNKNOWN') or r['ownUpload'].startswith('UNKNOWN'))),
        'upload-time N/A': sum(1 for r in elig if r['ownUpload'].startswith('N/A') and
                               not r['ownDraw'].startswith('UNKNOWN') and r['ownDraw'] != 'ALIASED'),
        'white-out skips': whiteout, 'no-opportunity': no_opp}
    own_decided = sum(1 for r in elig if r['ownDraw'] == 'CLEAN' and r['ownUpload'] == 'CLEAN')
    st_pos = sum(1 for r in elig if r['content'] in ('STALE-CONTENT', 'STALE-OWNER',
                                                    'RESIDENT-MISMATCH'))
    st_excl = {'UNKNOWN': sum(1 for r in elig if r['content'].startswith('UNKNOWN')),
               'non-upload residents': sum(1 for r in elig if r['content'].startswith('NONUPLOAD')),
               'white-out skips': whiteout, 'no-opportunity': no_opp}
    st_decided = sum(1 for r in elig if r['content'] == 'FRESH-SAMPLED')

    unreleased = sum(1 for lt in reg.lifetimes.values() if lt['end'] is None)
    summary = {
        'cohort': {'definition': 'every binding of a draw that reached the draw tap while the '
                                 'recorder was open',
                   'bindings': len(cohort), 'content': tally(elig, 'content'),
                   'ownershipAtDraw': tally(elig, 'ownDraw'),
                   'ownershipAtUpload': tally(elig, 'ownUpload'),
                   'keysStale': len({r['key'] for r in elig if r['content'].startswith('STALE')}),
                   'keysMismatch': len({r['key'] for r in elig if r['content'] == 'RESIDENT-MISMATCH'}),
                   'keysAliased': len({r['key'] for r in elig
                                       if 'ALIASED' in (r['ownDraw'], r['ownUpload'])})},
        'windowCounters': ({'drawCmds': close.x, 'drawsAtTap': close.y, 'bindingsSeen': close.w,
                            'whiteoutSkips': close.v, 'admissionRefused': close.x - close.y}
                           if close is not None else None),
        'verdicts': {H_OWN: verdict(own_pos, own_excl, own_decided),
                     H_STALE: verdict(st_pos, st_excl, st_decided)},
        'flaggedOutsideWindow': {'bindings': len(flg), 'content': tally(flg, 'content'),
                                 'ownershipAtDraw': tally(flg, 'ownDraw'),
                                 'note': 'flagged pages only, first 64 per key - not a rate'},
        'fetch': ('unproven' if ps_used is None else
                  'static ps-sampler map: fetch OPPORTUNITY, not execution'),
        'grants': {'registered': len(reg.lifetimes), 'neverSeenReleased': unreleased},
        'uploads': {'published': len(pubs), 'applied': len(applies),
                    'pendingAtClose': len(pending_at_close),
                    'pairedWithIssuedDecode': len(issued_of_pub)},
        'censusTapCost': ({'probes': cost.b, 'probeMs': cost.x / 1e6, 'maxProbeMs': cost.y / 1e6,
                           'decodeTapMs': cost.w / 1e6, 'drawTapMs': cost.v / 1e6,
                           'inFlightAtWrite': cost.c, 'flushWaitMs': cost.d,
                           'note': 'the taps\' own time; the census\'s effect on run timing is '
                                   'UNMEASURED'} if cost else None),
        'events': hdr['events'], 'overflow': hdr['overflow'],
    }
    return {'header': hdr, 'checks': checks,
            'failures': {k: v[:20] for k, v in fails.items() if v}, 'complete': complete,
            'witness': witness, 'witnessed': witnessed, 'draws': out, 'summary': summary}


def report(res):
    lines = []
    h = res['header'] or {}
    lines.append('events=%s overflow=%s lastSeq=%s' % (h.get('events'), h.get('overflow'),
                                                       h.get('lastSeq')))
    for k, v in res['checks'].items():
        lines.append('  %-10s %s' % (k, v))
        for f in res['failures'].get(k, [])[:5]:
            lines.append('      ' + f)
    lines.append('COMPLETE' if res['complete'] else
                 'INCOMPLETE - every verdict below is provisional')
    missing = [k for k, v in res.get('witness', {}).items() if not v]
    lines.append('WITNESSED (every hook fired; the trace joined)' if not missing else
                 'UNWITNESSED %s - an absence there proves nothing' % ' '.join(missing))
    s = res['summary']
    if not s:
        return '\n'.join(lines)
    c = s['cohort']
    lines.append('DECLARED COHORT: %s: %d bindings (fetch %s)' % (c['definition'], c['bindings'],
                                                                 s['fetch']))
    if s['windowCounters']:
        w = s['windowCounters']
        lines.append('  renderer counters: %d draw commands, %d at the tap (%d refused at '
                     'admission - outside the cohort), %d bindings seen, %d white-out skips' %
                     (w['drawCmds'], w['drawsAtTap'], w['admissionRefused'], w['bindingsSeen'],
                      w['whiteoutSkips']))
    for title, key in (('content', 'content'), ('ownership at draw publish', 'ownershipAtDraw'),
                       ('ownership at upload', 'ownershipAtUpload')):
        lines.append('  %s:' % title)
        for k, v in c[key].items():
            lines.append('    %7d  %s' % (v, k))
    lines.append('  distinct keys: stale=%d mismatch=%d aliased=%d' %
                 (c['keysStale'], c['keysMismatch'], c['keysAliased']))
    for hname, v in s['verdicts'].items():
        lines.append('%s: %s' % (hname, v))
    f = s['flaggedOutsideWindow']
    lines.append('outside the window (%s): %d bindings' % (f['note'], f['bindings']))
    for k, v in f['content'].items():
        lines.append('    %7d  %s' % (v, k))
    lines.append('grants: %d registered, %d never seen released (UNKNOWN at close)' %
                 (s['grants']['registered'], s['grants']['neverSeenReleased']))
    u = s['uploads']
    lines.append('uploads: published=%d applied=%d pendingAtClose=%d pairedWithIssuedDecode=%d' %
                 (u['published'], u['applied'], u['pendingAtClose'], u['pairedWithIssuedDecode']))
    t = s['censusTapCost']
    lines.append('census tap cost: %s' % ('NOT RECORDED' if not t else
                 '%d probes %.1f ms (max %.3f), decode taps %.1f ms, draw taps %.1f ms, in flight '
                 'at write %d (waited %d ms) - %s' %
                 (t['probes'], t['probeMs'], t['maxProbeMs'], t['decodeTapMs'], t['drawTapMs'],
                  t['inFlightAtWrite'], t['flushWaitMs'], t['note'])))
    return '\n'.join(lines)


# ── self-test: synthetic streams ─────────────────────────────────────────────
def _srht_module():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import srht_census as S
    return S


class Builder(object):
    """Emits a stream the way the game does: owners from the same Registry, a
    simulated FIFO clock, residents per band, window edges and counters, and
    the matching SRHT trace (live records from the open edge)."""

    def __init__(self):
        self.evs = []
        self.reg = Registry()
        self.serial = 0
        self.fifo = 0
        self.pub = 0
        self.app = 0
        self.res = [dict(), dict(), dict()]
        self.window = False
        self.counters = [0, 0, 0, 0]
        self.draw_fifos = set()
        self.open_z = None
        self.close_z = None

    def _add(self, t, a=0, b=0, c=0, d=0, e=0, f=0, g=0, x=0, y=0, z=0, w=0, v=0):
        ev = Ev((t, a, b, c, d, e, f, g, len(self.evs) + 1, x, y, z, w, v))
        self.evs.append(ev)
        return ev

    def grant(self, kind, va, size, p0=None, p1=None):
        self.serial += 1
        self.reg.register(kind, va, size, self.serial, len(self.evs) + 1)
        self._add(GREG, kind, va, size, self.serial, z=self.fifo if p0 is None else p0,
                  w=self.fifo if p1 is None else p1)
        return self.serial

    def release(self, kind, va, how):
        serial = self.reg.m[kind][va][1]
        self.reg.release(kind, va, serial, how, len(self.evs) + 1)
        self._add(GREL, kind, va, 0, serial, how, z=self.fifo, w=self.fifo)

    def decode(self, base, span, fmt, dims, decision, content, window=8, p0=None, p1=None,
               upload_fifo=0, flags=0):
        o = self.reg.owners(base, span)
        e = (decision | (min(o[0], 255) << 8) | ((o[1] & 0xFF) << 16) | ((window & 0xF) << 24) |
             flags | (DEC_ALIASED if o[5] else 0) | (0 if content else DEC_UNREADABLE))
        return self._add(DEC, base, span, fmt, dims, e, o[2], o[3], content, upload_fifo,
                         self.fifo if p0 is None else p0, self.fifo if p1 is None else p1, o[4])

    def publish(self, base, span, fmt, dims, payload_hash):
        self.fifo += 1
        self.pub += 1
        self._add(PUB, base, 0x100, span, dims, fmt, self.pub, x=payload_hash, z=self.fifo)
        return self.fifo

    def apply(self, base, fmt, dims, fifo, applied=True, band=BAND_2D):
        if applied:
            self.res[band][base] = (FROM_UPLOAD, self.app + 1)
            self._add(RES, base, FROM_UPLOAD, band, 0, 0, self.app + 1, z=fifo)
        self.app += 1
        self._add(APPLY, base, APPLIED if applied else 3, band, dims, fmt, self.app, z=fifo)

    def upload(self, base, span, fmt, dims, content, decode=True, applied=True, band=BAND_2D):
        p0 = self.fifo
        fifo = self.publish(base, span, fmt, dims, content)
        if decode:
            self.decode(base, span, fmt, dims, ISSUED, content, p0=p0, upload_fifo=fifo)
        self.apply(base, fmt, dims, fifo, applied, band)
        return fifo

    def resident_write(self, base, band, source):
        self.fifo += 1
        self.res[band][base] = (source, 0)
        self._add(RES, base, source, band, 0, 0, 0, z=self.fifo)

    def open_window(self, rec_open=3):
        self.window = True
        self.counters = [0, 0, 0, 0]
        self.fifo += 1                    # the opening Present executes (not recorded)
        self.open_z = self.fifo - 1
        self.rec_open = rec_open          # preamble records
        self._add(MARK, MARK_OPEN, rec_open & 0xFFFFFFFF, rec_open >> 32, z=self.open_z)

    def close_window(self, refused=0, whiteout=0):
        self.window = False
        for _ in range(refused):          # draw commands refused at admission
            self.fifo += 1
            self.draw_fifos.add(self.fifo)
            self.counters[0] += 1
        self.counters[2] += whiteout
        self.counters[3] += whiteout
        self.close_z = self.fifo
        live = self.close_z - self.open_z
        self._add(MARK, MARK_CLOSE, (self.rec_open + live) & 0xFFFFFFFF, 0,
                  x=self.counters[0], y=self.counters[1], z=self.fifo, w=self.counters[2],
                  v=self.counters[3])

    def draw(self, key, fmt, dims, E=None, dim=1, stacked=0, fetch_base=None):
        if E is None:
            if self.window:
                self.fifo += 1            # its SetTexture: a live, non-draw record
            self.fifo += 1
            E = self.fifo
        pop, srcs, ords = 0, [0, 0, 0], [0, 0, 0]
        for band in (BAND_2D, BAND_3D, BAND_CUBE):
            r = self.res[band].get(key)
            if r:
                pop |= 1 << band
                srcs[band], ords[band] = r
        if self.window:
            self.counters[0] += 1
            self.counters[1] += 1
            self.counters[2] += 1
            self.draw_fifos.add(E)
        e = ((fmt & 0x3F) | (1 << 8) | (dim << 9) | ((1 if self.window else 0) << 11) | (pop << 12) |
             (stacked << 15) | (srcs[0] << 16) | (srcs[1] << 20) | (srcs[2] << 24))
        self._add(DRAW, len(self.evs), 0, key, key if fetch_base is None else fetch_base, e, dims,
                  ords[0], x=0x77, y=ords[1] | (ords[2] << 32), z=E)

    def cost(self, inflight=0):
        self._add(MARK, MARK_COST, 5, inflight, 250 if inflight else 3, 1 if inflight else 0,
                  x=1000, y=300, w=5000, v=2000)

    def blob(self, **hdr):
        return write(hdr, self.evs)

    def srht(self, bad=None):
        S = _srht_module()
        recs = [S._rec(3, (0,) * 7) for _ in range(self.rec_open)]          # the preamble
        for fifo in range(self.open_z + 2, self.close_z + 2):             # live, incl. closing Present
            t = 10 if fifo in self.draw_fifos else (0 if fifo == self.close_z + 1 else 3)
            recs.append(S._rec(t, (0,) * 7))
        return S._trace(recs, version=7, bad=bad)


X, DX, Y, DY, S_ = 0x06, (256 << 16) | 256, 0x12, (128 << 16) | 128, 0x10000
S = S_
KEYS = {'stale_owner': 0x1000000, 'stale_content': 0x1100000, 'order_after': 0x1200000,
        'order_amb': 0x1250000, 'benign': 0x1300000, 'aliased': 0x1400000, 'cover': 0x1450000,
        'mismatch': 0x1500000, 'mismatch_dims': 0x1550000, 'mismatch_fmt': 0x1580000,
        'fresh': 0x1600000, 'unlock': 0x1700000, 'noresident': 0x1800000, 'nodecode': 0x1900000,
        'aliased_mismatch': 0x1A00000, 'unowned': 0x1B00000, 'unreadable': 0x1E00000,
        'multiband': 0x1F00000, 'third_owner': 0x2000000, 'steal': 0x2100000,
        'lossy': 0x2200000, 'grant_amb': 0x2400000, 'late_append': 0x2500000,
        'band_cube': 0x2600000, 'band_stacked': 0x2700000, 'fetch_base': 0x2800000,
        'many_owners': 0x2900000}


def scenario(skip=()):
    B = Builder()
    k = KEYS
    B.grant(2, 0xA0000000 | k['stale_owner'], S)
    B.upload(k['stale_owner'], S, X, DX, 0x11)
    B.release(2, 0xA0000000 | k['stale_owner'], 2)
    B.grant(2, 0xA0000000 | k['stale_owner'], S)
    B.decode(k['stale_owner'], S, X, DX, SUPPRESSED_CHANGED, 0x11)
    B.grant(2, 0xA0000000 | k['stale_content'], S)
    B.upload(k['stale_content'], S, X, DX, 0x31)
    B.decode(k['stale_content'], S, X, DX, SUPPRESSED_CHANGED, 0x32)
    B.grant(2, 0xA0000000 | k['benign'], S)
    B.upload(k['benign'], S, X, DX, 0x51)
    B.release(2, 0xA0000000 | k['benign'], 2)
    B.grant(2, 0xA0000000 | k['benign'], S)
    B.upload(k['benign'], S, Y, DY, 0x52)
    B.grant(1, 0x80000000 | k['aliased'], S)
    B.grant(2, 0xA0000000 | k['aliased'], S)
    B.upload(k['aliased'], S, X, DX, 0x61)
    B.grant(2, 0xA0000000 | k['cover'], S // 2)
    B.grant(2, 0xA0000000 | (k['cover'] + S // 2), S // 2)
    B.upload(k['cover'], S, X, DX, 0x62)
    for key, (f2, d2) in (('mismatch', (Y, DY)), ('mismatch_dims', (X, DY)), ('mismatch_fmt', (Y, DX))):
        B.grant(2, 0xA0000000 | k[key], S)
        B.upload(k[key], S, X, DX, 0x71)
        B.upload(k[key], S, f2, d2, 0x72)
    B.grant(1, 0x80000000 | k['aliased_mismatch'], S)
    B.grant(2, 0xA0000000 | k['aliased_mismatch'], S)
    B.upload(k['aliased_mismatch'], S, X, DX, 0xA1)
    B.upload(k['aliased_mismatch'], S, Y, DY, 0xA2)
    B.grant(2, 0xA0000000 | k['fresh'], S)
    B.upload(k['fresh'], S, X, DX, 0x81)
    B.upload(k['unowned'], S, X, DX, 0xB1)
    B.grant(2, 0xA0000000 | k['nodecode'], S)
    B.upload(k['nodecode'], S, X, DX, 0x91, decode=False)
    B.grant(2, 0xA0000000 | k['unreadable'], S)
    B.upload(k['unreadable'], S, X, DX, 0xE1)
    B.decode(k['unreadable'], S, X, DX, SUPPRESSED_CHANGED, 0)
    B.grant(2, 0xA0000000 | k['multiband'], S)
    B.upload(k['multiband'], S, X, DX, 0xF1)
    B.resident_write(k['multiband'], BAND_3D, FROM_RESOLVE)
    B.grant(1, 0x80000000 | k['third_owner'], S)
    B.grant(2, 0xA0000000 | k['third_owner'], S)
    B.grant(2, 0xC0000000 | k['third_owner'], S)
    B.upload(k['third_owner'], S, X, DX, 0xC1)
    B.release(2, 0xC0000000 | k['third_owner'], 2)
    B.grant(2, 0xC0000000 | k['third_owner'], S)
    B.decode(k['third_owner'], S, X, DX, SUPPRESSED_CHANGED, 0xC1)
    B.grant(2, 0xA0000000 | k['steal'], S)
    B.upload(k['steal'], S, X, DX, 0xD1)
    B.upload(k['steal'], S, X, DX, 0xD2, decode=False)
    B.grant(2, 0xA0000000 | k['lossy'], 0x140000)
    B.upload(k['lossy'], S, 6, DX, 0xE5)
    B.decode(k['lossy'], S, 2, (1280 << 16) | 256, NO_BASELINE, 0xE6)
    B.resident_write(k['unlock'], BAND_2D, FROM_UNLOCK)
    for name in ('order_after', 'order_amb', 'grant_amb', 'late_append', 'band_cube',
                 'band_stacked', 'fetch_base'):
        B.grant(2, 0xA0000000 | k[name], S)
        B.upload(k[name], S, X, DX, 0x40 + len(name))
    # 65 disjoint owners: exact aliasing, no overflow state
    for i in range(65):
        B.grant(2, 0xA0000000 | (k['many_owners'] + i * 0x1000), 0x1000)
    B.upload(k['many_owners'], 65 * 0x1000, X, DX, 0x99)
    B.draw(k['stale_owner'], X, DX)        # outside the window: one flagged draw
    B.open_window()
    for name in ('stale_owner', 'stale_content', 'aliased', 'cover', 'mismatch', 'mismatch_dims',
                 'mismatch_fmt', 'aliased_mismatch', 'fresh', 'unowned', 'nodecode', 'unreadable',
                 'multiband', 'third_owner', 'steal', 'lossy', 'unlock', 'noresident',
                 'many_owners'):
        if name in skip:
            continue
        B.draw(k[name], X, DX)
    B.draw(k['benign'], Y, DY)
    if 'band_cube' not in skip:
        B.draw(k['band_cube'], X, DX, dim=3)
    if 'band_stacked' not in skip:
        B.draw(k['band_stacked'], X, DX, dim=1, stacked=1)
    if 'fetch_base' not in skip:
        B.draw(k['fetch_base'], X, DX, fetch_base=k['fetch_base'] + 0x10000)
    # the change is observed after the draw's publish
    B.fifo += 1
    E = B.fifo
    B.decode(k['order_after'], S, X, DX, SUPPRESSED_CHANGED, 0x49)
    B.draw(k['order_after'], X, DX, E=E)
    # an observation that straddles the draw's publish
    p0 = B.fifo
    B.fifo += 1
    E = B.fifo
    B.decode(k['order_amb'], S, X, DX, SUPPRESSED_CHANGED, 0x4A, p0=p0, p1=E)
    B.draw(k['order_amb'], X, DX, E=E)
    # Astra 070's schedule: observed, E published and executed, THEN appended -
    # with P1 read at the append the bracket straddles E
    p0 = B.fifo
    B.fifo += 1
    E = B.fifo
    B.draw(k['late_append'], X, DX, E=E)
    B.decode(k['late_append'], S, X, DX, SUPPRESSED_CHANGED, 0x4B, p0=p0, p1=E)
    # a grant whose mutation straddles the draw's publish
    p0 = B.fifo
    B.fifo += 1
    E = B.fifo
    B.grant(1, 0x80000000 | k['grant_amb'], S, p0=p0, p1=E)
    B.draw(k['grant_amb'], X, DX, E=E)
    B.close_window()
    B.cost()
    return B


EXPECT = {  # key name -> (content, ownership at draw, ownership at upload)
    'stale_owner': ('STALE-OWNER', 'CLEAN', 'CLEAN'),
    'stale_content': ('STALE-CONTENT', 'CLEAN', 'CLEAN'),
    'benign': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'aliased': ('FRESH-SAMPLED', 'ALIASED', 'ALIASED'),
    'cover': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'mismatch': ('RESIDENT-MISMATCH', 'CLEAN', 'CLEAN'),
    'mismatch_dims': ('RESIDENT-MISMATCH', 'CLEAN', 'CLEAN'),
    'mismatch_fmt': ('RESIDENT-MISMATCH', 'CLEAN', 'CLEAN'),
    'aliased_mismatch': ('RESIDENT-MISMATCH', 'ALIASED', 'ALIASED'),
    'fresh': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'unowned': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'nodecode': ('UNKNOWN(no-issued-decode)', 'UNKNOWN(span-unknown)', 'UNKNOWN(no-issued-decode)'),
    'unreadable': ('UNKNOWN(content-unreadable)', 'CLEAN', 'CLEAN'),
    'multiband': ('UNKNOWN(multi-band)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    'third_owner': ('STALE-OWNER', 'ALIASED', 'ALIASED'),
    'steal': ('UNKNOWN(no-issued-decode)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    'lossy': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'unlock': ('NONUPLOAD(unlock)', 'UNKNOWN(span-unknown)', 'N/A(no-upload-observation)'),
    'noresident': ('UNKNOWN(no-resident)', 'UNKNOWN(span-unknown)', 'UNKNOWN(no-issued-decode)'),
    'order_after': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    'order_amb': ('UNKNOWN(order-ambiguous)', 'CLEAN', 'CLEAN'),
    'grant_amb': ('FRESH-SAMPLED', 'UNKNOWN(order-ambiguous)', 'CLEAN'),
    'late_append': ('UNKNOWN(order-ambiguous)', 'CLEAN', 'CLEAN'),
    'band_cube': ('UNKNOWN(band-mismatch)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    'band_stacked': ('UNKNOWN(band-mismatch)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    'fetch_base': ('UNKNOWN(fetch-base-mismatch)', 'UNKNOWN(fetch-base-mismatch)',
                   'UNKNOWN(fetch-base-mismatch)'),
    'many_owners': ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
}


def clean_window(extra=None):
    """A complete, witnessed window of one clean fresh binding (+ extra(B))."""
    B = Builder()
    B.grant(2, 0xA0000000 | 0x3200000, S)
    B.upload(0x3200000, S, X, DX, 0x21)
    if extra:
        extra(B, 'pre')
    B.open_window()
    B.draw(0x3200000, X, DX)
    if extra:
        extra(B, 'in')
    refused = extra(B, 'refused') if extra else 0
    whiteout = extra(B, 'whiteout') if extra else 0
    B.close_window(refused=refused or 0, whiteout=whiteout or 0)
    B.cost()
    return B


def self_test():
    fails = 0

    def chk(ok, name):
        nonlocal fails
        print('  %s  %s' % ('ok' if ok else 'FAIL:', name))
        if not ok:
            fails += 1

    B = scenario()
    res = analyze(B.blob(), srht=B.srht())
    chk(res['complete'], 'T0 the synthetic stream is complete (%s)' % res['failures'])
    chk(res['witnessed'], 'T0w the synthetic stream is witnessed, the trace joined (%s)' %
        res['witness'])
    rows = {r['key']: r for r in res['draws'] if r['window']}
    for i, (name, want) in enumerate(sorted(EXPECT.items())):
        r = rows.get(KEYS[name])
        got = (r['content'], r['ownDraw'], r['ownUpload']) if r else None
        chk(got == want, 'T%02d %s -> %s (got %s)' % (i + 1, name, want, got))
    chk('multi-cover' in rows[KEYS['cover']]['notes'] and
        'multi-cover' not in rows[KEYS['aliased']]['notes'],
        'F1 adjacent disjoint owners are noted multi-cover, never aliased')
    chk('reuse' in rows[KEYS['benign']]['notes'] and 'reuse' not in rows[KEYS['fresh']]['notes'],
        'F2 benign reuse is noted where an earlier owner ended before the current one')
    s = res['summary']
    chk(s['cohort']['bindings'] == len(EXPECT) and s['flaggedOutsideWindow']['bindings'] == 1,
        'F3 the declared cohort and the flagged outside sample are reported apart')
    chk(s['verdicts'][H_OWN].startswith('SURVIVES') and s['verdicts'][H_STALE].startswith('SURVIVES'),
        'F4 positives on both axes: both observations SURVIVE (not causes)')
    # zero positives with UNKNOWN bindings: only a NARROW negative
    B2 = scenario(skip=('stale_owner', 'stale_content', 'aliased', 'mismatch', 'mismatch_dims',
                        'mismatch_fmt', 'aliased_mismatch', 'third_owner', 'steal'))
    r2 = analyze(B2.blob(), srht=B2.srht())
    v = r2['summary']['verdicts']
    chk(r2['complete'] and v[H_STALE].startswith('NARROW') and v[H_OWN].startswith('NARROW'),
        'F5 zero positives with UNKNOWN bindings is a NARROW negative (%s)' % v)
    def with_unknown(B, phase):
        if phase == 'in':
            B.draw(0x3400000, X, DX)      # never uploaded: UNKNOWN on both axes, nothing else
        return 0
    B5b = clean_window(with_unknown)
    r5b = analyze(B5b.blob(), srht=B5b.srht())
    v5b = r5b['summary']['verdicts']
    chk(r5b['complete'] and r5b['witnessed'] and
        all(v5b[h].startswith('NARROW') and '1 UNKNOWN' in v5b[h] for h in (H_STALE, H_OWN)),
        'F5b an UNKNOWN binding - the ONLY exclusion - blocks a KILL on both axes (%s)' % v5b)
    # no ISSUED decode anywhere: unwitnessed -> no negative
    B3 = Builder()
    B3.grant(2, 0xA0000000 | 0x3000000, S)
    B3.upload(0x3000000, S, X, DX, 0x11, decode=False)
    B3.decode(0x3100000, S, X, DX, NO_BASELINE, 0x12)
    B3.open_window()
    B3.draw(0x3000000, X, DX)
    B3.close_window()
    B3.cost()
    r3 = analyze(B3.blob(), srht=B3.srht())
    chk(r3['complete'] and not r3['witness']['W-issued'] and
        r3['summary']['verdicts'][H_STALE].startswith('INCONCLUSIVE'),
        'F6 no ISSUED decode anywhere: UNWITNESSED, so no negative at all')
    B4 = clean_window()
    r4 = analyze(B4.blob(), srht=B4.srht())
    chk(r4['complete'] and r4['witnessed'] and
        r4['summary']['verdicts'][H_OWN].startswith('KILLED in the declared cohort') and
        r4['summary']['verdicts'][H_STALE].startswith('KILLED in the declared cohort'),
        'F7 a complete, witnessed, fully decided clean cohort is KILLED in the declared cohort (%s)'
        % r4['summary']['verdicts'])
    r4n = analyze(B4.blob())
    chk(not r4n['witness']['W-srht-join'] and
        r4n['summary']['verdicts'][H_OWN].startswith('INCONCLUSIVE'),
        'F7b ... but never without the SRHT join')

    def with_unlock(B, phase):
        if phase == 'pre':
            B.resident_write(0x3300000, BAND_2D, FROM_UNLOCK)
            B.decode(0x3300000, 0x1000, X, DX, NO_BASELINE, 0x22)
        elif phase == 'in':
            B.draw(0x3300000, X, DX)
        return 0
    B6 = clean_window(with_unlock)
    r6 = analyze(B6.blob(), srht=B6.srht())
    chk(r6['complete'] and r6['summary']['verdicts'][H_STALE].startswith('NARROW') and
        'non-upload' in r6['summary']['verdicts'][H_STALE] and
        r6['summary']['verdicts'][H_OWN].startswith('NARROW') and
        'upload-time N/A' in r6['summary']['verdicts'][H_OWN],
        'F8 a non-upload resident and an upload-time N/A are NAMED exclusions (%s)' %
        r6['summary']['verdicts'])

    def with_whiteout(B, phase):
        return 1 if phase == 'whiteout' else 0
    B9 = clean_window(with_whiteout)
    r9 = analyze(B9.blob(), srht=B9.srht())
    chk(r9['complete'] and r9['summary']['verdicts'][H_OWN].startswith('NARROW') and
        'white-out' in r9['summary']['verdicts'][H_OWN],
        'F9 a white-out skip inside the cohort blocks a KILL (%s)' % r9['summary']['verdicts'][H_OWN])

    def with_refused(B, phase):
        return 2 if phase == 'refused' else 0
    B10 = clean_window(with_refused)
    r10 = analyze(B10.blob(), srht=B10.srht())
    chk(r10['complete'] and r10['summary']['verdicts'][H_OWN].startswith('KILLED') and
        '2 draw(s) refused at admission' in r10['summary']['verdicts'][H_OWN],
        'F10 draws refused at admission are named OUTSIDE the cohort (%s)' %
        r10['summary']['verdicts'][H_OWN])
    r11 = analyze(B4.blob(), srht=B4.srht(), ps_map={'0x77': [1]})
    chk(r11['summary']['verdicts'][H_OWN].startswith('INCONCLUSIVE (no decided') or
        r11['summary']['verdicts'][H_OWN].startswith('NARROW'),
        'F11 a cohort the static map empties is never KILLED (%s)' %
        r11['summary']['verdicts'][H_OWN])

    # Completeness mutants: each must FAIL exactly the named check.
    def mutant(name, check, blob, srht=None):
        r = analyze(blob, srht=srht)
        chk(r['checks'].get(check) == 'FAIL' and not r['complete'],
            'M %s -> %s FAIL' % (name, check))

    base = scenario()
    ev = base.evs
    mutant('seq gap', 'C-seq', write({}, ev[:5] + ev[6:]))
    mutant('overflow', 'C-overflow', write({'overflow': 3}, ev))
    mutant('lastSeq short', 'C-seq', write({'lastSeq': len(ev) - 1}, ev))
    by = [0] * NTYPES
    for e in ev:
        by[e.type] += 1
    by[DRAW] += 1
    mutant('byType', 'C-bytype', write({'byType': by}, ev))
    mutant('truncated', 'C-header', write({}, ev)[:-10])
    mutant('trace magic corrupted', 'C-window', base.blob(), srht=base.srht(bad='magic'))
    mutant('trace record count lies', 'C-window', base.blob(), srht=base.srht(bad='records'))
    Bt = scenario()
    Bt.draw_fifos.add(next(f for f in range(Bt.open_z + 2, Bt.close_z + 1) if f not in Bt.draw_fifos))
    mutant('trace has a draw the census did not execute', 'C-window', Bt.blob(), srht=Bt.srht())

    def with_change(pred, fn, drop=False, srht=False):
        B2 = scenario()
        for i, e in enumerate(B2.evs):
            if pred(e):
                if drop:
                    del B2.evs[i]
                    for j, x in enumerate(B2.evs):
                        x.seq = j + 1
                else:
                    fn(e)
                break
        return (B2.blob(), B2.srht()) if srht else B2.blob()
    k = KEYS
    b, t = with_change(lambda e: e.type == DRAW and e.c == k['fresh'] and (e.e >> 11) & 1,
                       lambda e: setattr(e, 'z', e.z - 1), srht=True)
    mutant('a binding joined to a non-draw record', 'C-window', b, srht=t)
    mutant('inflight at write', 'C-inflight',
           with_change(lambda e: e.type == MARK and e.a == MARK_COST, lambda e: setattr(e, 'c', 1)))
    mutant('no cost mark', 'C-inflight',
           with_change(lambda e: e.type == MARK and e.a == MARK_COST, None, drop=True))
    mutant('apply z', 'C-lockstep', with_change(lambda e: e.type == APPLY and e.f == 2,
                                                lambda e: setattr(e, 'z', e.z + 1)))
    mutant('apply dims', 'C-lockstep', with_change(lambda e: e.type == APPLY and e.f == 3,
                                                   lambda e: setattr(e, 'd', e.d ^ 1)))
    mutant('render z backwards', 'C-zorder', with_change(lambda e: e.type == DRAW and e.c == k['cover'],
                                                         lambda e: setattr(e, 'z', 1)))
    # Each fixture isolates ONE check: the last decode's P0 rises past its own
    # P1 (no later decode, so the stream order still holds) ...
    B8 = scenario()
    last_dec = [e for e in B8.evs if e.type == DEC][-1]
    last_dec.z = last_dec.w + 5
    mutant('decode bracket inverted', 'C-zorder', B8.blob())
    # ... and a decode claims (P1) a publish appended AFTER it, with every later
    # decode's P1 already past that publish (so the stream order still holds).
    B9 = scenario()
    for i, e in enumerate(B9.evs):
        if e.type == PUB:
            prev = [x for x in B9.evs[:i] if x.type == DEC]
            later = [x for x in B9.evs[i:] if x.type == DEC]
            if prev and prev[-1].w < e.z and all(x.w >= e.z for x in later):
                prev[-1].w = e.z
                break
    mutant('decode read a later publish', 'C-zorder', B9.blob())
    mutant('P1 < E but appended after the binding', 'C-zorder',
           with_change(lambda e: e.type == DEC and e.a == k['late_append'] and
                       (e.e & 0xFF) == SUPPRESSED_CHANGED, lambda e: setattr(e, 'w', e.z)))
    B7 = scenario()
    decs = [e for e in B7.evs if e.type == DEC]
    decs[-1].z = decs[-2].z - 1                     # serialized observations: brackets never go back
    mutant('decode bracket goes backwards', 'C-zorder', B7.blob())
    mutant('recorded owner count', 'C-owners',
           with_change(lambda e: e.type == DEC and e.a == k['aliased'],
                       lambda e: setattr(e, 'e', (e.e & ~(0xFF << 8)) | (1 << 8))))
    mutant('recorded digest', 'C-owners',
           with_change(lambda e: e.type == DEC and e.a == k['third_owner'] and (e.e & 0xFF) == 2,
                       lambda e: setattr(e, 'v', e.v ^ 1)))
    mutant('recorded aliasing', 'C-owners',
           with_change(lambda e: e.type == DEC and e.a == k['cover'],
                       lambda e: setattr(e, 'e', e.e | DEC_ALIASED)))
    mutant('many owners called aliased', 'C-owners',
           with_change(lambda e: e.type == DEC and e.a == k['many_owners'],
                       lambda e: setattr(e, 'e', e.e | DEC_ALIASED)))
    mutant('release of an unknown grant', 'C-grants',
           with_change(lambda e: e.type == GREL, lambda e: setattr(e, 'd', 999)))
    mutant('ISSUED names another publish', 'C-pairing',
           with_change(lambda e: e.type == DEC and e.a == k['fresh'],
                       lambda e: setattr(e, 'y', e.y + 1)))
    mutant('ISSUED payload hash differs', 'C-pairing',
           with_change(lambda e: e.type == DEC and e.a == k['fresh'],
                       lambda e: setattr(e, 'x', e.x ^ 2)))
    mutant('upload slot write without its apply', 'C-resident',
           with_change(lambda e: e.type == APPLY and e.a == k['fresh'],
                       lambda e: setattr(e, 'b', 3)))
    mutant('applied upload with no slot write', 'C-resident',
           with_change(lambda e: e.type == RES and e.a == k['fresh'], None, drop=True))
    mutant('binding records a wrong resident', 'C-resident',
           with_change(lambda e: e.type == DRAW and e.c == k['fresh'] and (e.e >> 11) & 1,
                       lambda e: setattr(e, 'g', e.g + 1)))
    mutant('in-window binding dropped', 'C-window',
           with_change(lambda e: e.type == DRAW and e.c == k['fresh'] and (e.e >> 11) & 1,
                       None, drop=True))
    mutant('window bit disagrees with the edges', 'C-window',
           with_change(lambda e: e.type == DRAW and e.c == k['fresh'] and (e.e >> 11) & 1,
                       lambda e: setattr(e, 'e', e.e & ~(1 << 11))))
    mutant('close edge missing', 'C-window',
           with_change(lambda e: e.type == MARK and e.a == MARK_CLOSE, None, drop=True))
    mutant('live record count disagrees', 'C-window',
           with_change(lambda e: e.type == MARK and e.a == MARK_CLOSE, lambda e: setattr(e, 'b', e.b + 1)))
    B5 = scenario()
    B5.evs = [e for e in B5.evs if not (e.type == APPLY and e.f == 2)]
    for i, e in enumerate(B5.evs):
        e.seq = i + 1
    mutant('a publish never applied before a later apply', 'C-lockstep', B5.blob())
    r = analyze(with_change(lambda e: e.type == DEC and e.a == k['stale_owner'] and (e.e & 0xFF) == 2,
                            lambda e: setattr(e, 'f', e.f + 7)))
    rr = {x['key']: x for x in r['draws'] if x['window']}
    chk(rr[k['stale_owner']]['content'] == 'UNKNOWN(owner-chronology-mismatch)',
        'M an owner-chronology mismatch makes the content verdict UNKNOWN')
    r = analyze(scenario().blob(), ps_map={'0x77': [1]})
    chk(r['summary']['cohort']['content'] == {} and 'no-opportunity' in r['summary']['verdicts'][H_OWN]
        or r['summary']['verdicts'][H_OWN].startswith('INCONCLUSIVE'),
        'P1 a static map excludes bindings the shader has no opportunity to sample')
    r = analyze(scenario().blob(), ps_map={'0x77': [0]})
    chk(all(x['fetch'] == 'opportunity' for x in r['draws']) and 'OPPORTUNITY' in r['summary']['fetch'],
        'P2 ... and marks the rest a fetch OPPORTUNITY, not a fetch')
    print('[TEX_CENSUS_SELFTEST] %d failed' % fails)
    return fails


# The C++ suite's scenario keys (tools/difftest/tex_census_test.cpp) -> expected
# (content, ownership at draw, ownership at upload) for their in-window draws.
INTEGRATION_EXPECT = {
    0x0B600000: ('STALE-OWNER', 'CLEAN', 'CLEAN'),
    0x0B620000: ('STALE-CONTENT', 'CLEAN', 'CLEAN'),
    0x0B640000: ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    0x0B700000: ('FRESH-SAMPLED', 'ALIASED', 'ALIASED'),
    0x0B720000: ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    0x0B740000: ('RESIDENT-MISMATCH', 'CLEAN', 'CLEAN'),
    0x0B7C0000: ('RESIDENT-MISMATCH', 'ALIASED', 'ALIASED'),
    0x0B800000: ('UNKNOWN(no-resident)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    0x0BA80000: ('NONUPLOAD(unlock)', 'UNKNOWN(span-unknown)', 'N/A(no-upload-observation)'),
    0x0BAC0000: ('UNKNOWN(multi-band)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    0x0BB40000: ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    0x0BB80000: ('STALE-OWNER', 'ALIASED', 'ALIASED'),
    0x0B680000: ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    0x0B6A0000: ('UNKNOWN(order-ambiguous)', 'CLEAN', 'CLEAN'),
    0x0BC80000: ('UNKNOWN(order-ambiguous)', 'CLEAN', 'CLEAN'),
    0x0BCC0000: ('FRESH-SAMPLED', 'CLEAN', 'CLEAN'),
    0x0BD40000: ('UNKNOWN(band-mismatch)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    0x0BD80000: ('UNKNOWN(band-mismatch)', 'CLEAN', 'UNKNOWN(no-issued-decode)'),
    0x0BDC0000: ('UNKNOWN(fetch-base-mismatch)', 'UNKNOWN(fetch-base-mismatch)',
                 'UNKNOWN(fetch-base-mismatch)'),
}


def integration(difftest):
    fails = 0

    def chk(ok, name):
        nonlocal fails
        print('  %s  %s' % ('ok' if ok else 'FAIL:', name))
        if not ok:
            fails += 1
    p = subprocess.run([os.path.abspath(difftest), '--tex-census-test'], capture_output=True,
                       text=True, errors='replace')
    m = [l for l in p.stdout.splitlines() if l.startswith('[TXC_TEST]') and 'passed' in l]
    chk(p.returncode == 0 and m and m[-1].endswith(' 0 failed'),
        'I0 the C++ census suite passes (rc %d, %s)' % (p.returncode, m[-1] if m else 'no summary'))
    tmp = os.environ.get('TEMP', '.')
    with open(os.path.join(tmp, 'sr_txc_test.bin'), 'rb') as f:
        blob = f.read()
    srht_path = os.path.join(tmp, 'sr_txc_rec.srht')
    srht = open(srht_path, 'rb').read() if os.path.exists(srht_path) else None
    res = analyze(blob, srht=srht)
    chk(srht is not None, 'I1 the real recorder wrote the window trace')
    chk(res['complete'], 'I2 the stream written through the real FIFO, draw tap and recorder '
                         'window is COMPLETE (%s)' % res.get('failures'))
    chk(res['witnessed'], 'I3 ... and WITNESSED, the trace joined record-for-record (%s)' %
        res.get('witness'))
    rows = {r['key']: r for r in res['draws'] if r['window']}
    for key, want in sorted(INTEGRATION_EXPECT.items()):
        r = rows.get(key)
        got = (r['content'], r['ownDraw'], r['ownUpload']) if r else None
        chk(got == want, 'I %08X -> %s (got %s)' % (key, want, got))
    outside = [r for r in res['draws'] if not r['window'] and r['key'] == 0x0B600000]
    chk(len(outside) == 64 and all(r['content'] == 'STALE-OWNER' for r in outside),
        'I4 the 64 flagged draws outside the window are STALE-OWNER and outside the cohort')
    s = res['summary']
    chk(s['windowCounters'] == {'drawCmds': 20, 'drawsAtTap': 19, 'bindingsSeen': 19,
                                'whiteoutSkips': 0, 'admissionRefused': 1},
        'I5 the renderer\'s window counters conserve the 19 bindings and name the one draw refused '
        'at admission (%s)' % s['windowCounters'])
    c = s['censusTapCost']
    chk(c is not None and c['probes'] > 0 and c['inFlightAtWrite'] == 0,
        'I6 the census reports its own tap cost and nothing in flight at the write (%s)' % c)
    chk(s['verdicts'][H_OWN].startswith('SURVIVES') and s['verdicts'][H_STALE].startswith('SURVIVES'),
        'I7 both bounded hypotheses report their positives as SURVIVES (%s)' % s['verdicts'])
    print('[TEX_CENSUS_INTEGRATION] %d failed' % fails)
    return fails


def main(argv):
    if len(argv) >= 2 and argv[1] == '--self-test':
        return 1 if self_test() else 0
    if len(argv) >= 3 and argv[1] == '--integration':
        return 1 if integration(argv[2]) else 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    out_json, ps_map, srht = None, None, None
    i = 2
    while i < len(argv):
        if argv[i] == '--json':
            out_json = argv[i + 1]
        elif argv[i] == '--ps-sampler-map':
            with open(argv[i + 1]) as f:
                ps_map = json.load(f)
        elif argv[i] == '--srht':
            with open(argv[i + 1], 'rb') as f:
                srht = f.read()
        else:
            print('unknown argument %s' % argv[i])
            return 2
        i += 2
    if not os.path.exists(path):
        print('NO CENSUS FILE %s - no acquisition: every question is UNKNOWN' % path)
        return 3
    with open(path, 'rb') as f:
        res = analyze(f.read(), ps_map, srht)
    print(report(res))
    if out_json:
        with open(out_json, 'w') as f:
            dump = dict(res)
            dump['draws'] = res['draws'][:20000]
            dump['drawsTruncated'] = len(res['draws']) > 20000
            json.dump(dump, f, indent=1)
    return 0 if res['complete'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
