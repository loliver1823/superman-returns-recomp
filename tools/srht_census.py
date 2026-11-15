"""srht_census.py — offline structural census of a .srht HLE command trace.

GENERAL by construction: the trace path, record window and draw selector are all
arguments. Nothing about any particular key, surface, shader, texture or era is
encoded. Read-only — this tool never writes to a trace.

Companion to tools/srht_texkey.py, which inspects/rewrites ONE texture key's
payload. This one answers the ORDERING and CONTRACT questions a per-key payload
view cannot.

  verify <trace>
      Structural validation BEFORE any census is believed: magic, version, the
      header's record count and payload-byte total against what actually walks,
      per-record payload bounds, and trailing bytes. Every other subcommand runs
      this first and refuses to report a census over a trace that fails it.

  resolves <trace> [--log RUNLOG]
      Every Resolve in executor order, and a per-destination summary. Three
      counts are kept SEPARATE and never conflated:
        attempts   — Resolve records naming this destination
        eligible   — attempts that pass the executor's STRUCTURAL predicate
                     (full-zero or explicit-full rect, at origin, level and
                     slice zero).  gfx/hle_video.cpp
        backendEntries — attempts that REACHED the backend copy site. NOT
                     DERIVABLE OFFLINE, and NOT a success count: the runtime's
                     `eN` bucket increments BEFORE hleResolveToTexture is
                     called, so a copy that the backend then refuses for a
                     missing source, a disabled gate, no descriptor slot, a
                     failed create or a closed command list is still counted
                     here. Per-key SUCCESS is not published by any instrument
                     today. Reported as "?" unless --log supplies the runtime
                     [S6_RESOLVE_DESTS] census.
      Sampling evidence is likewise kept separate:
        uploads    — TextureUpload records for the key
        binds      — SetTexture records for the key (the guest ACTUALLY
                     sampling it), normalized &0x1FFFFFFF exactly as
                     gfx/hle_video.cpp:1153 normalizes it
      ⚠ A destination can be bound many times with zero uploads, or uploaded and
      never bound. Reading "bound" off the upload count is the mistake that made
      an earlier version of this tool report a sampled destination as unsampled.

  writers <trace>
      For every Resolve destination, the LAST WRITER — defined as the last
      operation that could have changed the destination's pixels. A REFUSED
      resolve is NOT a writer and is never reported as one; refused attempts are
      listed separately as refusals. Because neither success nor backend entry
      is derivable offline, the writer is reported as the last STRUCTURALLY
      ELIGIBLE operation, and labelled as such.

  window <trace> <fromRec> <toRec> [-v]
      Dump a record range: state-changing commands in full plus a per-type
      census. Use it to read what happened BETWEEN two resolves.

  draw <trace> (--rec N | --shader VSHEX:PSHEX) [--all]
      THE FULL CONTRACT of a draw, for auditing one specific draw without
      inferring anything from geometry alone: record and trace draw ordinal,
      primitive type and count, the raw vertex payload with the vertex
      DECLARATION applied, VS/PS hashes, VS/PS constant sizes, render target and
      depth surface, viewport and scissor, and the decoded render-state block
      (colour mask, blend control, depth control, VTE, viewport scale/offset).
      ⚠ `--rec`/ordinal here is the TRACE RECORD ordinal. It is NOT the same
      space as SR_DRAWID_SUPPRESS, whose ordinal is assigned at hleSubmitDraw
      entry and therefore skips every draw the backend rejects earlier. To
      cross-reference a suppression result, match on --shader identity, not on
      a number.

  selftest [--keep DIR]
      Build adversarial fixtures in a temp dir and assert this tool's
      classifications AND its printed output: uploaded-but-never-bound,
      bound-without-upload, refused-after-eligible-resolve, no-eligible-writer,
      an unsupported trace VERSION, cumulative-log snapshot selection, the
      present-interval axis agreeing across subcommands, the formatted summary
      line itself, and six corrupt-header classes. The passed/total figure is
      DERIVED from the assertion counter and printed — never written by hand.

Trace layout (gfx/hle_video.{hpp,cpp}):
  header 32B : magic[4]"SRHT", u32 version, u64 records, u64 payloadBytes,
               u32 presents, u32 reserved
  record 48B : RenderCommand (u32 type + 7 u32), u32 payloadBytes,
               u32 reserved, u64 payloadHash, then payloadBytes of payload

The type indices and per-command field order MIRROR the enum and tagged union in
gfx/hle_video.hpp. That enum is APPEND-ONLY by contract, so this tool stays
correct as long as nothing is inserted; if a census prints nonsense, check that
contract FIRST.
"""
import argparse
import collections
import os
import re
import struct
import sys
import tempfile

HDR = struct.Struct('<4sIQQII')
REC = struct.Struct('<8I')
TAIL = struct.Struct('<IIQ')
HDR_BYTES, REC_BYTES = 32, 48
KEY_MASK = 0x1FFFFFFF

# Mirrors RenderCommandType (gfx/hle_video.hpp:34) — APPEND ONLY.
NAMES = ['Present', 'Clear', 'ClearF', 'SetRenderTarget', 'SetDepthStencilSurface',
         'SetViewport', 'SetScissorRect', 'SetStreamSource', 'SetIndices',
         'SetTexture', 'DrawVertices', 'DrawIndexedVertices', 'BeginVertices',
         'EndVertices', 'TextureUnlock', 'SetShaders', 'SetVertexDecl',
         'SetConstants', 'TextureUpload', 'SetRenderState', 'Resolve',
         'DrawVerticesUP', 'TextureObjectEvent', 'OwnershipRestore',
         'VertexBufferPublish']
(T_PRESENT, T_CLEAR, T_CLEARF, T_SETRT, T_SETDS, T_SETVP, T_SETSC, T_SETSTREAM,
 T_SETIDX, T_SETTEX, T_DRAWV, T_DRAWI, T_BEGINV, T_ENDV, T_TEXUNLOCK, T_SETSH,
 T_SETDECL, T_SETCONST, T_TEXUP, T_SETRS, T_RESOLVE, T_DRAWUP, T_TEXOBJ,
 T_OWNRESTORE, T_VBPUB) = range(25)
T_DRAWS = (T_DRAWV, T_DRAWI, T_DRAWUP)

# Xenos PrimitiveType (Xenia src/xenia/gpu/xenos.h). ⚠ 13 is kQuadList, NOT
# RECTLIST — RECTLIST is 8. An earlier revision of this tool and of
# docs/MIGRATION_API_HLE.md called prim 13 "RECTLIST", which misnames the shape
# every UI/UP draw in this engine uses. Director-verified against Xenia: 8 =
# kRectangleList, 13 = kQuadList. The remaining entries are transcribed from the
# same enum and are NOT independently verified here.
PRIM = {0: 'kNone', 1: 'kPointList', 2: 'kLineList', 3: 'kLineStrip',
        4: 'kTriangleList', 5: 'kTriangleFan', 6: 'kTriangleStrip',
        7: 'kTriangleWithWFlags', 8: 'kRectangleList', 12: 'kLineLoop',
        13: 'kQuadList', 14: 'kQuadStrip', 15: 'kPolygon'}


def prim_name(p):
    return PRIM.get(p, f'unknown({p})')


# Render-state block layout (gfx/hle_video.cpp:441-452). The two register runs
# are contiguous in MEMORY, not in register index: run 0x2100 is 26 dwords, so
# reg 0x2200 sits at dword 26.
RS_RUN2_BASE = 26
RS_MIN_BYTES = 236


def rs_idx1(reg):
    return reg - 0x2100


def rs_idx2(reg):
    return RS_RUN2_BASE + (reg - 0x2200)


def rs_be32(block, dword_idx):
    o = dword_idx * 4
    if o + 4 > len(block):
        return None
    return struct.unpack_from('>I', block, o)[0]


def rs_bef(block, dword_idx):
    v = rs_be32(block, dword_idx)
    return None if v is None else struct.unpack('<f', struct.pack('<I', v))[0]


# ── structural validation ───────────────────────────────────────────────────

class TraceError(Exception):
    pass


def read_header(data):
    if len(data) < HDR_BYTES:
        raise TraceError(f'file is {len(data)} bytes, shorter than the 32-byte header')
    magic, ver, nrec, npay, presents, reserved = HDR.unpack_from(data, 0)
    if magic != b'SRHT':
        raise TraceError(f'bad magic {magic!r} — not a .srht trace')
    return dict(magic=magic, version=ver, records=nrec, payloadBytes=npay,
                presents=presents, reserved=reserved)


# The ONLY version this tool can interpret. A v4 trace predates the fields the
# censuses read, so decoding one with v5 field offsets produces confident
# nonsense rather than an error — an earlier revision printed the version and
# ACCEPTED a hand-built v4 fixture with problems=[] while its own docstring
# claimed the version was validated.
SUPPORTED_VERSION = 5
# iter-458 (Astra 016): the versioned-layout audit (gfx/hle_video.hpp, the
# kTraceVersion comment block). The RECORD ENVELOPE is identical v5..v7; the
# versions differ only in what may appear:
#   v6 adds VertexBufferPublish (type 24). A v5 trace carries NO publications,
#      so a type-24 record in a v5 trace is refused, never interpreted.
#   v7 adds the CUBE TextureUpload: fmtBits texDim (bits 14-15) == 3, a second
#      payload region, mipBytes in the union's 7th dword (args[7]), which a
#      v5/v6 producer never wrote (indeterminate stack bytes). A v5/v6 record
#      CLAIMING a cube upload is refused BY NAME; in v7 its payload must be
#      exactly base bytes (args[3]) + mipBytes.
#   Types 22 TextureObjectEvent / 23 OwnershipRestore were appended during the
#   v5 era and keep their values in every version.
#   v8 (Astra 086) adds the ClearF COLOUR PAYLOAD: a ClearF record may carry
#      exactly 16 bytes (one float4) as its payload, with colorBytes (args[5])
#      == 16. A v5..v7 ClearF carries NO payload (a v7 reader would drop it and
#      execute the record with its live-session colorBlock), so a pre-v8 ClearF
#      record WITH a payload is refused by name, and a v8 one must be exactly
#      16/16.
# None of this tool's subcommands decode any field that changed meaning, so the
# census semantics are unchanged across v5..v8.
SUPPORTED_VERSIONS = (5, 6, 7, 8)
CLEARF_COLOR_BYTES = 16
TEXDIM_SHIFT, TEXDIM_CUBE = 14, 3


def walk(data, strict=True):
    """Yield (index, cmd tuple, payloadBytes, bodyOffset). Raises on overrun."""
    off, idx = HDR_BYTES, 0
    n = len(data)
    while off + REC_BYTES <= n:
        cmd = REC.unpack_from(data, off)
        pbytes, _res, _phash = TAIL.unpack_from(data, off + 32)
        body = off + REC_BYTES
        if body + pbytes > n:
            if strict:
                raise TraceError(
                    f'record {idx} claims {pbytes} payload bytes but only '
                    f'{n - body} remain before EOF')
            return
        yield idx, cmd, pbytes, body
        off = body + pbytes
        idx += 1
    if strict and off != n:
        raise TraceError(f'{n - off} trailing bytes after the last complete record')


def validate(data, quiet=False):
    """Full structural validation. Returns (header, problems[])."""
    problems = []
    h = read_header(data)
    ver = h['version']
    if ver not in SUPPORTED_VERSIONS:
        problems.append(
            f'version {ver} is not supported — this tool interprets the '
            f'v{SUPPORTED_VERSIONS[0]}..v{SUPPORTED_VERSIONS[-1]} layouts ONLY, '
            f'and decoding another version with them yields confident nonsense')
    seen_records = 0
    seen_payload = 0
    seen_presents = 0
    types = collections.Counter()
    try:
        for idx, cmd, pb, _body in walk(data, strict=True):
            seen_records += 1
            seen_payload += pb
            t = cmd[0]
            types[t] += 1
            if t == T_PRESENT:
                seen_presents += 1
            if t >= len(NAMES):
                problems.append(f'record {idx}: unknown command type {t} '
                                f'(trace newer than this tool?)')
            elif t == T_VBPUB and ver < 6:
                problems.append(f'record {idx}: VertexBufferPublish in a '
                                f'v{ver} trace (introduced in v6)')
            elif t == T_TEXUP and ((cmd[5] >> TEXDIM_SHIFT) & 3) == TEXDIM_CUBE:
                if ver < 7:
                    problems.append(f'record {idx}: a v{ver} TextureUpload '
                                    f'CLAIMS a cube (fmtBits={cmd[5]:08X}); '
                                    f'cube uploads exist only in v7')
                elif pb != cmd[3] + cmd[7]:
                    problems.append(f'record {idx}: v7 cube TextureUpload '
                                    f'payload {pb} != base {cmd[3]} + '
                                    f'mipBytes {cmd[7]}')
            elif t == T_CLEARF and pb:
                if ver < 8:
                    problems.append(f'record {idx}: a v{ver} ClearF CARRIES a '
                                    f'{pb}-byte payload; ClearF colour '
                                    f'payloads exist only in v8')
                elif pb != CLEARF_COLOR_BYTES or cmd[5] != CLEARF_COLOR_BYTES:
                    problems.append(f'record {idx}: v8 ClearF payload {pb} / '
                                    f'colorBytes {cmd[5]} - both must be '
                                    f'{CLEARF_COLOR_BYTES}')
    except TraceError as e:
        problems.append(str(e))
    if h['records'] != seen_records:
        problems.append(f'header says records={h["records"]}, walked {seen_records}')
    if h['payloadBytes'] != seen_payload:
        problems.append(f'header says payloadBytes={h["payloadBytes"]}, '
                        f'summed {seen_payload}')
    if h['presents'] != seen_presents:
        problems.append(f'header says presents={h["presents"]}, counted {seen_presents}')
    if not quiet:
        print(f'header  : v{h["version"]} records={h["records"]} '
              f'payloadBytes={h["payloadBytes"]} presents={h["presents"]} '
              f'fileBytes={len(data)}')
        print(f'walked  : records={seen_records} payloadBytes={seen_payload} '
              f'presents={seen_presents}')
    return h, problems


def load(path, quiet=False):
    """Read + validate. Refuses to hand back a structurally invalid trace."""
    with open(path, 'rb') as f:
        data = f.read()
    if not quiet:
        print(f'trace   : {path}')
    h, problems = validate(data, quiet=quiet)
    if problems:
        print('\n⚠ STRUCTURAL PROBLEMS — census output over this trace is NOT '
              'trustworthy:', file=sys.stderr)
        for p in problems:
            print(f'  - {p}', file=sys.stderr)
        raise TraceError(f'{len(problems)} structural problem(s)')
    return data, h


# ── resolve decode ──────────────────────────────────────────────────────────

def decode_resolve(a):
    """Unpack the resolve union and apply the executor's STRUCTURAL predicate.

    Eligibility here means only "the executor would attempt the copy". It does
    NOT mean the copy ran — see the module docstring.
    """
    src, destKey, destSize, rectLT, rectRB, destPoint, lsf = a
    dw = (destSize & 0x1FFF) + 1
    dh = ((destSize >> 13) & 0x1FFF) + 1
    level, slc = lsf & 0xF, (lsf >> 4) & 0xFF
    px, py = destPoint & 0xFFFF, destPoint >> 16
    fullZero = rectLT == 0 and rectRB == 0
    explicitFull = (rectLT == 0 and destPoint == 0
                    and (rectRB & 0xFFFF) == dw and (rectRB >> 16) == dh)
    atOrigin = px == 0 and py == 0
    if (not fullZero and not explicitFull) or not atOrigin:
        cls, refusal = 'REFUSED', 'rect'
    elif level or slc:
        cls, refusal = 'REFUSED', 'sliced'
    else:
        cls, refusal = 'ELIGIBLE', None
    return dict(src=src, destKey=destKey, key=destKey & KEY_MASK, dw=dw, dh=dh,
                rl=rectLT & 0xFFFF, rt=rectLT >> 16,
                rr=rectRB & 0xFFFF, rb=rectRB >> 16, px=px, py=py,
                level=level, slice=slc, srcSel=(lsf >> 12) & 7,
                clearRT=(lsf >> 15) & 1, clearZ=(lsf >> 16) & 1,
                cls=cls, refusal=refusal)


def parse_runlog(path):
    """Pull the runtime per-destination census from a run log.

    [S6_RESOLVE_DESTS] KEY(eN/rN/sN) ... — `e` counts attempts that REACHED
    the backend copy site. ⚠ IT IS NOT A SUCCESS COUNT: hle_video.cpp
    increments `pk.exec` BEFORE calling hleResolveToTexture, and the refusal
    the backend may then return is only aggregated globally
    (s_resolveSkipOther), never per key. Reporting `e` as "succeeded" overstates
    it. Per-key success is not published by any instrument today.

    ⚠ The line is emitted PER PRESENT and its counters are CUMULATIVE, so the
    LAST occurrence in a log is not necessarily the final state — a log may end
    mid-run, or the emitter may be gated off for later presents. Taking the last
    line reported 4 exec for a key with 18 attempts. Keep the per-key MAXIMUM
    total instead, which is correct for monotonic counters, and let the caller
    cross-check it against the offline attempt count.
    """
    out = {}
    pat = re.compile(r'([0-9A-Fa-f]{8})\(e(\d+)/r(\d+)/s(\d+)\)')
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                if '[S6_RESOLVE_DESTS]' not in line:
                    continue
                for m in pat.finditer(line):
                    k = int(m.group(1), 16) & KEY_MASK
                    cand = dict(exec=int(m.group(2)), rect=int(m.group(3)),
                                sliced=int(m.group(4)))
                    cand['total'] = cand['exec'] + cand['rect'] + cand['sliced']
                    prev = out.get(k)
                    if prev is None or cand['total'] > prev['total']:
                        out[k] = cand
    except OSError as e:
        print(f'⚠ could not read --log {path}: {e}', file=sys.stderr)
    return out


# ── subcommands ─────────────────────────────────────────────────────────────

def cmd_verify(a):
    with open(a.trace, 'rb') as f:
        data = f.read()
    print(f'trace   : {a.trace}')
    try:
        _h, problems = validate(data)
    except TraceError as e:
        print(f'\nINVALID: {e}')
        return 2
    if problems:
        print('\nINVALID:')
        for p in problems:
            print(f'  - {p}')
        return 2
    print('\nVALID: header, record count, payload total and presents all agree.')
    return 0


def collect(data):
    """One pass gathering everything the censuses need."""
    present = 0
    rt0 = depth = 0
    draws_since_bind = 0
    resolves = []
    presents = []
    rt_binds = []
    uploads = collections.Counter()     # normkey -> TextureUpload records
    binds = collections.Counter()       # normkey -> SetTexture records
    events = collections.defaultdict(list)
    draw_ord = 0
    for idx, c, pb, body in walk(data):
        t, args = c[0], c[1:]
        if t == T_PRESENT:
            presents.append((present, args[0], args[1], args[3], args[4]))
            present += 1
        elif t == T_SETRT:
            if args[1] == 0:
                rt0, draws_since_bind = args[2], 0
                rt_binds.append((idx, present, args[2]))
        elif t == T_SETDS:
            depth = args[1]
        elif t in T_DRAWS:
            draws_since_bind += 1
            draw_ord += 1
        elif t == T_SETTEX:
            if args[2]:
                binds[args[2] & KEY_MASK] += 1
        elif t == T_TEXUP:
            k = args[0] & KEY_MASK
            uploads[k] += 1
            events[k].append((idx, present, 'UPLOAD',
                              f'bytes={args[2]} {args[3] >> 16}x{args[3] & 0xFFFF} '
                              f'fmtBits={args[4]:X} pitch={args[5]}'))
        elif t == T_RESOLVE:
            r = decode_resolve(args)
            r.update(rec=idx, present=present, rt0=rt0, depth=depth,
                     draws=draws_since_bind)
            resolves.append(r)
            kind = ('RESOLVE-eligible' if r['cls'] == 'ELIGIBLE'
                    else f'RESOLVE-REFUSED({r["refusal"]})')
            events[r['key']].append(
                (idx, present, kind,
                 f'src={r["src"]:08X} {r["dw"]}x{r["dh"]} '
                 f'lvl/sl={r["level"]}/{r["slice"]}'))
    return dict(resolves=resolves, presents=presents, rt_binds=rt_binds,
                uploads=uploads, binds=binds, events=events, ndraws=draw_ord)


def cmd_resolves(a):
    data, _h = load(a.trace)
    st = collect(data)
    rows = st['resolves']
    runtime = parse_runlog(a.log) if a.log else {}

    print(f'\nresolves={len(rows)} presents={len(st["presents"])} '
          f'drawRecords={st["ndraws"]} '
          f'distinctUploadedKeys={len(st["uploads"])} '
          f'distinctBoundKeys={len(st["binds"])}')
    if not runtime:
        print('backend-entry counts: NOT AVAILABLE (pass --log <runlog> to read '
              'the runtime [S6_RESOLVE_DESTS] census). Per-key SUCCESS is not '
              'published by any instrument today, with or without a log.')

    print('\n=== ORDERED RESOLVES (structural classification only) ===')
    print(f'{"#":>4} {"rec":>7} {"pres":>4} {"src":>8} {"sel":>3} {"destKey":>8} '
          f'{"dest":>9} {"rect":>19} {"point":>9} {"lv/sl":>6} {"cRT":>3} '
          f'{"draws":>6} {"class":>9} {"why":>7}')
    for i, r in enumerate(rows):
        rect = f'({r["rl"]},{r["rt"]})-({r["rr"]},{r["rb"]})'
        pt = f'({r["px"]},{r["py"]})'
        print(f'{i+1:>4} {r["rec"]:>7} {r["present"]:>4} {r["src"]:08X} '
              f'{r["srcSel"]:>3} {r["destKey"]:08X} {r["dw"]:>4}x{r["dh"]:<4} '
              f'{rect:>19} {pt:>9} {r["level"]}/{r["slice"]:<4} {r["clearRT"]:>3} '
              f'{r["draws"]:>6} {r["cls"]:>9} {r["refusal"] or "-":>7}')

    print('\n=== PER DESTINATION ===')
    print('attempts = Resolve records | eligible = pass the structural predicate')
    print('backendEntries = attempts that REACHED the copy site (runtime '
          'only; NOT successes) | uploads vs binds are DIFFERENT evidence')
    byDest = collections.OrderedDict()
    for i, r in enumerate(rows):
        byDest.setdefault(r['destKey'], []).append((i + 1, r))
    for dk, lst in byDest.items():
        k = dk & KEY_MASK
        elig = [(n, r) for n, r in lst if r['cls'] == 'ELIGIBLE']
        ref = collections.Counter(r['refusal'] for _n, r in lst if r['refusal'])
        rt = runtime.get(k)
        succ = '?'
        stale = ''
        if rt:
            succ = str(rt['exec'])
            if rt['total'] < len(lst):
                # The runtime census saw fewer attempts than the trace holds, so
                # it is a MID-RUN snapshot for this key and its exec count is a
                # lower bound, not the answer.
                succ += '+'
                stale = (f'  ⚠ runtime census saw only {rt["total"]}/{len(lst)} '
                         f'attempts for this key — MID-RUN SNAPSHOT, exec is a '
                         f'lower bound')
        refstr = ' '.join(f'{k2}={v}' for k2, v in sorted(ref.items())) or '-'
        n_last, last = lst[-1]
        print(f'{dk:08X} (norm {k:08X}) {lst[0][1]["dw"]}x{lst[0][1]["dh"]:<5} '
              f'attempts={len(lst):<3} eligible={len(elig):<3} '
              f'backendEntries={succ:<4} refused[{refstr}] '
              f'| uploads={st["uploads"].get(k, 0):<3} '
              f'binds={st["binds"].get(k, 0):<4} '
              f'| lastAttempt=#{n_last}({last["cls"]}) '
              f'lastEligible=#{elig[-1][0] if elig else "-"}')
        if stale:
            print(stale)

    print('\n=== PRESENTS ===')
    fk = collections.Counter()
    for p, dev, front, key, dims in st['presents']:
        fk[key] += 1
        print(f'present {p}: device={dev:08X} front={front:08X} '
              f'frontKey={key:08X} frontDims={dims:08X} '
              f'({(dims & 0x1FFF) + 1}x{((dims >> 13) & 0x1FFF) + 1})')
    # How many present INTERVALS actually carry a resolve to the presented key?
    print('\n=== FRONT-KEY RESOLVE COVERAGE ===')
    npres = len(st['presents'])
    for key, cnt in fk.most_common():
        k = key & KEY_MASK
        ivs = sorted({r['present'] for r in rows
                      if r['key'] == k and r['cls'] == 'ELIGIBLE'})
        print(f'frontKey {key:08X}: declared by {cnt}/{npres} presents; '
              f'eligible resolves in {len(ivs)}/{npres} present intervals '
              f'{ivs}')
        if len(ivs) < npres:
            print(f'  ⚠ NOT every present interval resolves into this key — do '
                  f'not describe it as "the last resolve of every frame".')

    print(f'\n=== RT0 BINDINGS ({len(st["rt_binds"])}) ===')
    for recIdx, p, s in st['rt_binds'][:a.max_binds]:
        print(f'  rec {recIdx:>7} present {p} RT0 <- {s:08X}')
    if len(st['rt_binds']) > a.max_binds:
        print(f'  ... {len(st["rt_binds"]) - a.max_binds} more (--max-binds)')
    return 0


def cmd_writers(a):
    data, _h = load(a.trace)
    st = collect(data)
    events = st['events']
    dest_keys = []
    for r in st['resolves']:
        if r['key'] not in dest_keys:
            dest_keys.append(r['key'])

    print(f'\n{len(dest_keys)} resolve destinations\n')
    print('A REFUSED resolve is NOT a writer. "writer" below means the last')
    print('STRUCTURALLY ELIGIBLE operation — whether the backend copy actually')
    print('ran is not derivable from the trace.\n')
    overwritten = []
    never = []
    for k in dest_keys:
        ev = sorted(events[k])
        writers = [e for e in ev if e[2] in ('UPLOAD',) or e[2] == 'RESOLVE-eligible']
        refusals = [e for e in ev if e[2].startswith('RESOLVE-REFUSED')]
        if not writers:
            never.append(k)
            print(f'{k:08X}: NO ELIGIBLE WRITER AT ALL '
                  f'({len(refusals)} refused attempts, '
                  f'binds={st["binds"].get(k, 0)})')
            continue
        last = writers[-1]
        flag = ''
        if last[2] == 'UPLOAD' and any(w[2] == 'RESOLVE-eligible' for w in writers):
            flag = '   <<< UPLOAD AFTER AN ELIGIBLE RESOLVE'
            overwritten.append(k)
        print(f'{k:08X}: eligibleResolves='
              f'{sum(1 for w in writers if w[2] == "RESOLVE-eligible")} '
              f'uploads={sum(1 for w in writers if w[2] == "UPLOAD")} '
              f'refused={len(refusals)} binds={st["binds"].get(k, 0)} | '
              f'LAST ELIGIBLE WRITER @rec{last[0]} present{last[1]} = '
              f'{last[2]} ({last[3]}){flag}')
        for e in ev[-a.tail:]:
            print(f'          rec{e[0]:>7} p{e[1]} {e[2]:<26} {e[3]}')
        if refusals and refusals[-1][0] > last[0]:
            print(f'          ⚠ {sum(1 for r in refusals if r[0] > last[0])} '
                  f'REFUSED attempt(s) AFTER the last eligible writer — this '
                  f'destination is STALE relative to what the guest asked for.')
    print(f'\ndestinations whose last eligible writer is an UPLOAD, not a '
          f'resolve: {len(overwritten)}/{len(dest_keys)}')
    for k in overwritten:
        print(f'  {k:08X}')
    print(f'destinations with NO eligible writer at all: {len(never)}/{len(dest_keys)}')
    for k in never:
        print(f'  {k:08X}')
    return 0


def cmd_window(a):
    data, _h = load(a.trace)
    counts = collections.Counter()
    print(f'\n=== RECORDS {a.fromRec}..{a.toRec} ===')
    for idx, c, _pb, _body in walk(data):
        if idx < a.fromRec:
            continue
        if idx > a.toRec:
            break
        t, args = c[0], c[1:]
        nm = NAMES[t] if t < len(NAMES) else f'?{t}'
        counts[nm] += 1
        detail = None
        if t == T_PRESENT:
            detail = f'frontKey={args[3]:08X} front={args[1]:08X}'
        elif t == T_CLEAR:
            detail = (f'count={args[1]} flags={args[3]:08X} color={args[4]:08X}'
                      f'   [0-3=TARGET0..3, 0x10=ZBUFFER, 0x20=STENCIL]')
        elif t == T_CLEARF:
            detail = (f'flags={args[1]:08X} colorBlock={args[3]} '
                      f'colorBytes={args[4]}'
                      f'   [0-3=TARGET0..3, 0x10=ZBUFFER, 0x20=STENCIL]')
        elif t == T_SETRT:
            detail = f'index={args[1]} surface={args[2]:08X}'
        elif t == T_SETDS:
            detail = f'surface={args[1]:08X}'
        elif t == T_SETVP:
            detail = f'({args[1]},{args[2]}) {args[3]}x{args[4]}'
        elif t == T_SETSC:
            detail = f'({args[1]},{args[2]})-({args[3]},{args[4]})'
        elif t == T_SETTEX:
            detail = (f'sampler={args[1]} texture={args[2]:08X} '
                      f'(key {args[2] & KEY_MASK:08X})')
        elif t == T_SETSH:
            detail = f'vs={args[2]:08X}{args[1]:08X} ps={args[4]:08X}{args[3]:08X}'
        elif t in T_DRAWS:
            detail = (f'prim={args[1]}({prim_name(args[1])}) start={args[2]} '
                      f'count={args[3]} vertBytes={args[5]} stride={args[6]}')
        elif t == T_RESOLVE:
            r = decode_resolve(args)
            detail = (f'src={r["src"]:08X} destKey={r["destKey"]:08X} '
                      f'{r["dw"]}x{r["dh"]} {r["cls"]}'
                      f'{"/" + r["refusal"] if r["refusal"] else ""} '
                      f'clearRT={r["clearRT"]} clearZ={r["clearZ"]}')
        if detail is not None and (a.verbose or t not in T_DRAWS):
            print(f'rec {idx:>7} {nm:<24} {detail}')
    print('\n--- window census by type ---')
    for k, v in counts.most_common():
        print(f'  {k:<26} {v}')
    return 0


DECL_ELEM_BYTES = 12


def decode_decl(payload, count):
    """12-byte D3DVERTEXELEMENT-ish records, big-endian.

    Layout per gfx/d3d12_backend.cpp:3709-3716:
      d0 = be32(e+0): offset = d0 & 0xFFFF, stream = d0 >> 16
      d1 = be32(e+4): type
      d2 = be32(e+8): usage = d2 >> 16, usageIndex = (d2 >> 8) & 0xFF
    """
    out = []
    for i in range(count):
        e = payload[i * DECL_ELEM_BYTES:(i + 1) * DECL_ELEM_BYTES]
        if len(e) < DECL_ELEM_BYTES:
            break
        d0, d1, d2 = struct.unpack('>III', e)
        out.append(dict(i=i, off=d0 & 0xFFFF, stream=d0 >> 16, type=d1,
                        usage=d2 >> 16, usageIndex=(d2 >> 8) & 0xFF, raw=e))
    return out


# D3DDECLUSAGE (the usage field's meaning is the standard D3D9 enum).
USAGE = {0: 'POSITION', 1: 'BLENDWEIGHT', 2: 'BLENDINDICES', 3: 'NORMAL',
         4: 'PSIZE', 5: 'TEXCOORD', 6: 'TANGENT', 7: 'BINORMAL',
         8: 'TESSFACTOR', 9: 'POSITIONT', 10: 'COLOR', 11: 'FOG',
         12: 'DEPTH', 13: 'SAMPLE'}


def cmd_draw(a):
    data, _h = load(a.trace)
    want_vs = want_ps = None
    if a.shader:
        try:
            vs_s, ps_s = a.shader.split(':')
            want_vs, want_ps = int(vs_s, 16), int(ps_s, 16)
        except ValueError:
            print('--shader expects VSHEX:PSHEX (16 hex digits each)',
                  file=sys.stderr)
            return 2

    st = dict(decl=None, const=None, rs=None, vp=None, sc=None,
              vs=0, ps=0, rt={}, ds=0, tex={})
    hits = []
    draw_ord = 0
    for idx, c, pb, body in walk(data):
        t, args = c[0], c[1:]
        if t == T_PRESENT:
            st['present'] = st.get('present', 0) + 1
        elif t == T_SETRT:
            st['rt'][args[1]] = args[2]
        elif t == T_SETDS:
            st['ds'] = args[1]
        elif t == T_SETVP:
            st['vp'] = args[1:7]
        elif t == T_SETSC:
            st['sc'] = args[1:5]
        elif t == T_SETTEX:
            st['tex'][args[1]] = args[2]
        elif t == T_SETSH:
            st['vs'] = (args[2] << 32) | args[1]
            st['ps'] = (args[4] << 32) | args[3]
        elif t == T_SETDECL:
            st['decl'] = (idx, args[3], data[body:body + pb])
        elif t == T_SETCONST:
            st['const'] = (idx, args[1], args[2], args[3], args[4], pb)
        elif t == T_SETRS:
            st['rs'] = (idx, args[2], data[body:body + pb])
        elif t in T_DRAWS:
            draw_ord += 1
            match = (idx == a.rec) if a.rec is not None else (
                st['vs'] == want_vs and st['ps'] == want_ps)
            if match:
                snap = dict(st)
                snap['rt'] = dict(st['rt'])
                snap['tex'] = dict(st['tex'])
                hits.append((idx, args, pb, body, draw_ord, snap))
                if not a.all:
                    break
    if not hits:
        print('no draw matched.', file=sys.stderr)
        return 1

    for (idx, args, pb, body, dord, s) in hits:
        _dev, prim, start, count, _arena, vbytes, stride = args
        print(f'\n{"=" * 72}')
        # ⚠ PRESENT INDEX CONVENTION, shared with `resolves`: the index is the
        # number of Presents already emitted, i.e. the index of the Present that
        # will END this interval. cmd_draw subtracted one and so labelled the
        # same three draws 1/4/6 where the resolve census called them 2/5/7 —
        # two subcommands of one tool disagreeing about their own axis.
        print(f'DRAW  rec={idx}  presentInterval={s.get("present", 0)}  '
              f'traceDrawOrdinal={dord}')
        print(f'{"=" * 72}')
        print(f'  primType      : {prim} ({prim_name(prim)})   count={count} start={start}')
        print(f'  vertBytes     : {vbytes}  stride={stride}  '
              f'payloadBytes={pb}  '
              f'verts={vbytes // stride if stride else "n/a"}')
        print(f'  VS            : {s["vs"]:016X}')
        print(f'  PS            : {s["ps"]:016X}')
        print(f'  RT0           : {s["rt"].get(0, 0):08X}   '
              f'RT1={s["rt"].get(1, 0):08X} RT2={s["rt"].get(2, 0):08X} '
              f'RT3={s["rt"].get(3, 0):08X}')
        print(f'  Depth surface : {s["ds"]:08X}')
        if s['vp']:
            vp = s['vp']
            minz = struct.unpack('<f', struct.pack('<I', vp[4]))[0]
            maxz = struct.unpack('<f', struct.pack('<I', vp[5]))[0]
            print(f'  Viewport      : ({vp[0]},{vp[1]}) {vp[2]}x{vp[3]} '
                  f'minZ={minz} maxZ={maxz}')
        if s['sc']:
            print(f'  Scissor       : {s["sc"]}')
        if s['const']:
            c0 = s['const']
            print(f'  SetConstants  : @rec{c0[0]} vsBytes={c0[2]} '
                  f'psBytes={c0[4]} payload={c0[5]}')

        # ── render state block ──
        if s['rs']:
            rrec, rbytes, blk = s['rs']
            print(f'\n  RENDER STATE  : @rec{rrec} bytes={rbytes} '
                  f'payload={len(blk)}')
            if len(blk) >= RS_MIN_BYTES:
                cm = rs_be32(blk, rs_idx1(0x2104))
                dc = rs_be32(blk, rs_idx2(0x2200))
                bc = rs_be32(blk, rs_idx2(0x2201))
                vte = rs_be32(blk, rs_idx2(0x2206))
                print(f'    RB_COLOR_MASK   (0x2104) = {cm:08X}  '
                      f'channels written: '
                      f'{"".join(n for n, b in zip("RGBA", range(4)) if cm >> b & 1) or "NONE"}'
                      f'   [4 bits per RT; RT0 = bits 0-3]')
                print(f'    RB_DEPTHCONTROL (0x2200) = {dc:08X}  '
                      f'zEnable={dc >> 1 & 1} zWrite={dc >> 2 & 1} '
                      f'zFunc={dc >> 4 & 7}')
                print(f'    RB_BLENDCONTROL0(0x2201) = {bc:08X}  '
                      f'colorSrc={bc & 0x1F} colorComb={bc >> 5 & 7} '
                      f'colorDst={bc >> 8 & 0x1F} '
                      f'alphaSrc={bc >> 16 & 0x1F} alphaComb={bc >> 21 & 7} '
                      f'alphaDst={bc >> 24 & 0x1F}')
                vcls = ('UNWRITTEN' if vte == 0 else
                        'clip-space(0x43F class)' if vte & 1 else
                        'pixel-space(0x400 class)' if vte & 0x400 else 'other')
                print(f'    PA_CL_VTE_CNTL  (0x2206) = {vte:08X}  [{vcls}]')
                sc = [rs_bef(blk, rs_idx1(r)) for r in (0x210F, 0x2111, 0x2113)]
                of = [rs_bef(blk, rs_idx1(r)) for r in (0x2110, 0x2112, 0x2114)]
                print(f'    PA_CL_VPORT scale = {sc}')
                print(f'    PA_CL_VPORT offset= {of}')
            else:
                print(f'    ⚠ block is {len(blk)} bytes, below the {RS_MIN_BYTES}'
                      f'-byte minimum the executor requires — NOT decoded')

        # ── vertex declaration + payload ──
        raw = data[body:body + pb]
        print(f'\n  VERTEX DECL   : ', end='')
        if s['decl']:
            drec, dcount, dpay = s['decl']
            els = decode_decl(dpay, dcount)
            print(f'@rec{drec} elems={dcount} payload={len(dpay)}B')
            for e in els:
                print(f'    el{e["i"]}: off={e["off"]:<4} stream={e["stream"]} '
                      f'type=0x{e["type"]:06X} '
                      f'usage={e["usage"]}({USAGE.get(e["usage"], "?")}) '
                      f'usageIdx={e["usageIndex"]}  raw={e["raw"].hex()}')
        else:
            els = []
            print('NONE IN FORCE — the draw cannot be laid out')

        print(f'\n  VERTEX PAYLOAD ({len(raw)} bytes, {"%d verts" % (vbytes // stride) if stride else "?"}):')
        print(f'    raw: {raw.hex()}')
        if stride and els:
            nv = min(vbytes // stride, len(raw) // stride)
            for v in range(nv):
                base = v * stride
                parts = []
                for e in els:
                    o = base + e['off']
                    nxt = min((x['off'] for x in els if x['off'] > e['off']),
                              default=stride)
                    w = nxt - e['off']
                    b = raw[o:o + w]
                    if not b:
                        continue
                    hexs = b.hex()
                    alts = []
                    if len(b) >= 4:
                        alts.append('be_u32=%08X' % struct.unpack_from('>I', b)[0])
                        alts.append('be_2i16=%s' % (struct.unpack_from('>2h', b),))
                        alts.append('bytes=%s' % (tuple(b[:4]),))
                    if len(b) >= 8:
                        alts.append('be_2f32=%s' % (struct.unpack_from('>2f', b),))
                    parts.append(f'{USAGE.get(e["usage"], e["usage"])}'
                                 f'{e["usageIndex"]}[{hexs}] ' + ' '.join(alts))
                print(f'    v{v}: ' + ' | '.join(parts))
            print('    ⚠ multiple numeric readings are printed on purpose: the '
                  'declaration TYPE field is not decoded to a D3DDECLTYPE here, '
                  'so which reading is correct is NOT established by this tool.')

        print(f'\n  SAMPLED TEXTURES (SetTexture state in force):')
        any_tex = False
        for slot in sorted(s['tex']):
            va = s['tex'][slot]
            if not va:
                continue
            any_tex = True
            print(f'    sampler {slot:>2}: {va:08X}  (key {va & KEY_MASK:08X})')
        if not any_tex:
            print('    (none)')
        print('    ⚠ binding is not sampling: whether the PS reads a given slot '
              'is a property of the shader, not of this list.')
    return 0


# ── adversarial fixtures + selftest ─────────────────────────────────────────

def _rec(t, args7, payload=b''):
    cmd = REC.pack(t, *args7)
    tail = TAIL.pack(len(payload), 0, 0)
    return cmd + tail + payload


def _trace(records, version=5, bad=None):
    body = b''.join(records)
    nrec = len(records)
    npay = sum(len(r) - REC_BYTES for r in records)
    npres = 0
    for r in records:
        if REC.unpack_from(r, 0)[0] == T_PRESENT:
            npres += 1
    if bad == 'records':
        nrec += 7
    if bad == 'payload':
        npay += 999
    if bad == 'presents':
        npres += 3
    if bad == 'magic':
        return b'XXXX' + HDR.pack(b'SRHT', version, nrec, npay, npres, 0)[4:] + body
    hdr = HDR.pack(b'SRHT', version, nrec, npay, npres, 0)
    out = hdr + body
    if bad == 'truncate':
        out = out[:-4]
    if bad == 'trailing':
        out = out + b'\x00\x00\x00\x00'
    return out


def _resolve_args(destKey, dw=64, dh=64, src=0x83A00000, rectLT=0, rectRB=0,
                  point=0, level=0, slc=0):
    destSize = ((dw - 1) & 0x1FFF) | (((dh - 1) & 0x1FFF) << 13)
    lsf = (level & 0xF) | ((slc & 0xFF) << 4)
    return (src, destKey, destSize, rectLT, rectRB, point, lsf)


def cmd_selftest(a):
    tmp = a.keep or tempfile.mkdtemp(prefix='srht_selftest_')
    os.makedirs(tmp, exist_ok=True)
    failures = []
    ran = []

    def check(name, cond, detail=''):
        # ⚠ The total is DERIVED from this counter and printed by the summary.
        # It is never written by hand: an earlier submission claimed "22/22"
        # for a suite that ran 20 assertions, which is the same
        # quote-a-number-you-did-not-produce defect this tool exists to catch.
        ran.append(name)
        if cond:
            print(f'  PASS  {name}')
        else:
            print(f'  FAIL  {name}  {detail}')
            failures.append(name)

    print(f'fixtures in {tmp}\n')

    # F1 — uploaded but never bound; and bound without upload.
    K_UP, K_BIND = 0x0AAA0000, 0x0BBB0000
    recs = [
        _rec(T_TEXUP, (K_UP, 0, 16, (4 << 16) | 4, 0, 0, 0), b'\x01' * 16),
        _rec(T_SETRT, (0, 0, 0x83A00000, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K_UP)),
        _rec(T_SETTEX, (0, 0, K_BIND, 0, 0, 0, 0)),
        _rec(T_SETTEX, (0, 1, K_BIND, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K_BIND)),
        _rec(T_PRESENT, (0, 0, 0, K_UP, 0, 0, 0)),
    ]
    p1 = os.path.join(tmp, 'f1_upload_vs_bind.srht')
    open(p1, 'wb').write(_trace(recs))
    d1, _ = load(p1, quiet=True)
    st1 = collect(d1)
    print('F1 uploaded-but-never-bound / bound-without-upload')
    check('F1 uploaded key has 1 upload', st1['uploads'].get(K_UP) == 1,
          f'got {st1["uploads"].get(K_UP)}')
    check('F1 uploaded key has 0 binds', st1['binds'].get(K_UP, 0) == 0,
          f'got {st1["binds"].get(K_UP, 0)}')
    check('F1 bound key has 2 binds', st1['binds'].get(K_BIND) == 2,
          f'got {st1["binds"].get(K_BIND)}')
    check('F1 bound key has 0 uploads', st1['uploads'].get(K_BIND, 0) == 0,
          f'got {st1["uploads"].get(K_BIND, 0)}')

    # F2 — refused AFTER a successful (eligible) resolve: the destination is
    # stale relative to what the guest asked for, and the refusal must NOT be
    # reported as a writer.
    K = 0x0CCC0000
    recs = [
        _rec(T_SETRT, (0, 0, 0x83A00000, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K)),                        # eligible
        _rec(T_SETTEX, (0, 0, K, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K, slc=1)),                 # refused: sliced
        _rec(T_RESOLVE, _resolve_args(K, rectLT=(3 << 16) | 3, rectRB=8)),  # refused: rect
        _rec(T_PRESENT, (0, 0, 0, K, 0, 0, 0)),
    ]
    p2 = os.path.join(tmp, 'f2_refused_after_success.srht')
    open(p2, 'wb').write(_trace(recs))
    d2, _ = load(p2, quiet=True)
    st2 = collect(d2)
    rs2 = st2['resolves']
    print('\nF2 refused-after-successful-resolve')
    check('F2 three attempts', len(rs2) == 3, f'got {len(rs2)}')
    check('F2 exactly one eligible',
          sum(1 for r in rs2 if r['cls'] == 'ELIGIBLE') == 1)
    check('F2 refusal reasons are sliced + rect',
          sorted(r['refusal'] for r in rs2 if r['refusal']) == ['rect', 'sliced'],
          f'got {sorted(r["refusal"] for r in rs2 if r["refusal"])}')
    ev = sorted(st2['events'][K])
    writers = [e for e in ev if e[2] == 'RESOLVE-eligible' or e[2] == 'UPLOAD']
    refusals = [e for e in ev if e[2].startswith('RESOLVE-REFUSED')]
    check('F2 exactly one writer', len(writers) == 1, f'got {len(writers)}')
    check('F2 refusals are not writers',
          all(not e[2].startswith('RESOLVE-REFUSED') for e in writers))
    check('F2 a refusal follows the last writer',
          bool(refusals) and refusals[-1][0] > writers[-1][0])
    check('F2 key is bound once', st2['binds'].get(K) == 1)

    # F3 — a destination with NO eligible writer at all (every attempt refused).
    K = 0x0DDD0000
    recs = [
        _rec(T_SETRT, (0, 0, 0x83A00000, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K, slc=2)),
        _rec(T_RESOLVE, _resolve_args(K, slc=3)),
        _rec(T_SETTEX, (0, 0, K, 0, 0, 0, 0)),
        _rec(T_PRESENT, (0, 0, 0, 0, 0, 0, 0)),
    ]
    p3 = os.path.join(tmp, 'f3_no_eligible_writer.srht')
    open(p3, 'wb').write(_trace(recs))
    d3, _ = load(p3, quiet=True)
    st3 = collect(d3)
    ev3 = sorted(st3['events'][K])
    print('\nF3 destination with no eligible writer')
    check('F3 zero eligible writers',
          not [e for e in ev3 if e[2] == 'RESOLVE-eligible'])
    check('F3 but it IS sampled', st3['binds'].get(K) == 1)

    # F4 — corrupt headers, four classes. Each must be REFUSED by load().
    print('\nF4 corrupt headers')
    good = [
        _rec(T_SETRT, (0, 0, 0x83A00000, 0, 0, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(0x0EEE0000)),
        _rec(T_PRESENT, (0, 0, 0, 0x0EEE0000, 0, 0, 0)),
    ]
    for bad in ('records', 'payload', 'presents', 'magic', 'truncate', 'trailing'):
        p = os.path.join(tmp, f'f4_{bad}.srht')
        open(p, 'wb').write(_trace(good, bad=bad))
        try:
            load(p, quiet=True)
            check(f'F4 {bad} rejected', False, 'load() ACCEPTED a corrupt trace')
        except TraceError:
            check(f'F4 {bad} rejected', True)
        except Exception as e:                                    # noqa: BLE001
            check(f'F4 {bad} rejected', False, f'wrong exception {e!r}')

    # F6 — the two subcommands must agree on the PRESENT INTERVAL axis. `draw`
    # once reported 1/4/6 where `resolves` reported 2/5/7 for the same three
    # commands, which is a tool disagreeing with itself.
    K = 0x0FFF0000
    recs = [
        _rec(T_PRESENT, (0, 0, 0, 0, 0, 0, 0)),
        _rec(T_PRESENT, (0, 0, 0, 0, 0, 0, 0)),
        _rec(T_SETRT, (0, 0, 0x83A00000, 0, 0, 0, 0)),
        _rec(T_SETSH, (0, 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0, 0)),
        _rec(T_RESOLVE, _resolve_args(K)),
        _rec(T_DRAWUP, (0, 13, 0, 4, 0, 8, 8), b'\x00' * 8),
        _rec(T_PRESENT, (0, 0, 0, K, 0, 0, 0)),
    ]
    p6 = os.path.join(tmp, 'f6_present_axis.srht')
    open(p6, 'wb').write(_trace(recs))
    d6, _ = load(p6, quiet=True)
    st6 = collect(d6)
    resolve_interval = st6['resolves'][0]['present']
    import contextlib
    import io as _io
    buf = _io.StringIO()
    ns = argparse.Namespace(trace=p6, rec=None,
                            shader='2222222211111111:4444444433333333', all=False)
    with contextlib.redirect_stdout(buf):
        cmd_draw(ns)
    m = re.search(r'presentInterval=(\d+)', buf.getvalue())
    draw_interval = int(m.group(1)) if m else -1
    print('\nF6 present-interval axis agrees across subcommands')
    check('F6 resolve interval is 2', resolve_interval == 2,
          f'got {resolve_interval}')
    check('F6 draw interval matches resolve interval',
          draw_interval == resolve_interval,
          f'draw={draw_interval} resolve={resolve_interval}')

    # F7 — an UNSUPPORTED VERSION must be rejected. A v4 trace is structurally
    # well-formed; only the version field distinguishes it, and decoding it with
    # v5 field offsets produces confident nonsense. An earlier revision accepted
    # exactly this fixture with problems=[].
    print('\nF7 unsupported trace version')
    for ver in (0, 1, 4, 9, 99):
        pv = os.path.join(tmp, f'f7_version{ver}.srht')
        open(pv, 'wb').write(_trace(good, version=ver))
        try:
            load(pv, quiet=True)
            check(f'F7 v{ver} rejected', False, 'load() ACCEPTED it')
        except TraceError:
            check(f'F7 v{ver} rejected', True)

    # F8 — cumulative-log selection. The [S6_RESOLVE_DESTS] line is emitted per
    # present with CUMULATIVE counters, so the LAST occurrence in a log is not
    # necessarily the largest: a log may end mid-run or the emitter may be gated
    # off later. Taking the last line reported exec=4 for a key with 18
    # attempts. parse_runlog must keep the per-key MAXIMUM.
    logp = os.path.join(tmp, 'f8_cumulative.log')
    with open(logp, 'w', encoding='utf-8') as f:
        f.write('[BOOT] [S6_RESOLVE_DESTS] 0ABC0000(e1/r0/s0) 0DEF0000(e5/r1/s0)\n')
        f.write('[BOOT] [S6_RESOLVE_DESTS] 0ABC0000(e9/r2/s3) 0DEF0000(e7/r1/s2)\n')
        f.write('[BOOT] [S6_RESOLVE_DESTS] 0ABC0000(e4/r0/s0) 0DEF0000(e2/r0/s0)\n')
    rl = parse_runlog(logp)
    print('\nF8 cumulative-log snapshot selection')
    check('F8 takes the maximum, not the last, for key A',
          rl[0x0ABC0000]['exec'] == 9, f'got {rl.get(0x0ABC0000)}')
    check('F8 takes the maximum, not the last, for key B',
          rl[0x0DEF0000]['exec'] == 7, f'got {rl.get(0x0DEF0000)}')
    check('F8 totals are summed for the max test',
          rl[0x0ABC0000]['total'] == 14, f'got {rl.get(0x0ABC0000)}')

    # F9 — the FORMATTED OUTPUT itself, not just the underlying values. The
    # defects that reached the director twice were both display-layer: a
    # hand-written assertion total, and two subcommands disagreeing about the
    # present axis. Assert what is actually printed.
    import contextlib
    import io as _io
    print('\nF9 formatted output')
    bufr = _io.StringIO()
    nsr = argparse.Namespace(trace=p2, log=None, max_binds=40)
    with contextlib.redirect_stdout(bufr):
        cmd_resolves(nsr)
    out = bufr.getvalue()
    check('F9 resolves prints backendEntries, never "succeeded"',
          'backendEntries=' in out and 'succeeded=' not in out)
    check('F9 resolves states per-key success is unpublished',
          'Per-key SUCCESS is not published' in out)
    check('F9 refusal class is named in the row', 'sliced=1' in out and 'rect=1' in out,
          'refusal breakdown missing')
    bufd = _io.StringIO()
    nsd = argparse.Namespace(trace=p6, rec=None,
                             shader='2222222211111111:4444444433333333', all=False)
    with contextlib.redirect_stdout(bufd):
        cmd_draw(nsd)
    outd = bufd.getvalue()
    check('F9 draw names prim 13 kQuadList, not RECTLIST',
          'kQuadList' in outd and 'RECTLIST' not in outd, outd[:200])
    check('F9 draw prints presentInterval', 'presentInterval=' in outd)

    # F10 (iter-458, Astra 016) - the versioned-layout gates. Each fixture is
    # structurally well-formed; only the version/type/cube contract differs.
    print('\nF10 versioned layouts v5..v8')
    cube_fmt = TEXDIM_CUBE << TEXDIM_SHIFT
    base_pl, mip_pl = b'\x02' * 32, b'\x03' * 16
    cube_ok = _rec(T_TEXUP, (0x0C000000, 0, 32, (4 << 16) | 4, cube_fmt, 0, 16),
                   base_pl + mip_pl)
    cube_bad = _rec(T_TEXUP, (0x0C000000, 0, 32, (4 << 16) | 4, cube_fmt, 0, 99),
                    base_pl + mip_pl)
    vbpub = _rec(T_VBPUB, (0x0D000000, 0, 16, 0, 0, 0, 0), b'\x04' * 16)
    ownr = _rec(T_OWNRESTORE, (0x0E000000, 0, 0, 0, 0, 0, 0))
    texobj = _rec(T_TEXOBJ, (0x0E000000, 1, 0, 0, 0, 0, 0))
    unknown = _rec(25, (0, 0, 0, 0, 0, 0, 0))
    # ClearF: device, flags, rect, colorBlock, colorBytes, z, stencil
    clr_ok = _rec(T_CLEARF, (0x839F2880, 0x3F, 0, 0, 16, 0, 0), b'\x05' * 16)
    clr_short = _rec(T_CLEARF, (0x839F2880, 0x3F, 0, 0, 16, 0, 0), b'\x05' * 12)
    clr_cmd12 = _rec(T_CLEARF, (0x839F2880, 0x3F, 0, 0, 12, 0, 0), b'\x05' * 16)
    clr_none = _rec(T_CLEARF, (0x839F2880, 0x30, 0, 0xFFFFFFFF, 0, 0, 0))
    cases = [
        ('F10 v6 good trace accepted', good, 6, True),
        ('F10 v7 good trace accepted', good, 7, True),
        ('F10 v6 VertexBufferPublish accepted', good + [vbpub], 6, True),
        ('F10 v5 VertexBufferPublish REFUSED', good + [vbpub], 5, False),
        ('F10 v7 types 22/23/24 accepted', good + [texobj, ownr, vbpub], 7, True),
        ('F10 v7 cube upload (base+mip) accepted', good + [cube_ok], 7, True),
        ('F10 v7 cube payload != base+mipBytes REFUSED', good + [cube_bad], 7, False),
        ('F10 v6 cube claim REFUSED', good + [cube_ok], 6, False),
        ('F10 v5 cube claim REFUSED', good + [cube_ok], 5, False),
        ('F10 v7 unknown type 25 REFUSED', good + [unknown], 7, False),
        ('F10 v8 good trace accepted', good, 8, True),
        ('F10 v8 ClearF 16-byte colour accepted', good + [clr_ok, clr_none], 8, True),
        ('F10 v8 ClearF 12-byte payload REFUSED', good + [clr_short], 8, False),
        ('F10 v8 ClearF colorBytes 12 w/ 16-byte payload REFUSED', good + [clr_cmd12], 8, False),
        ('F10 v7 ClearF WITH a payload REFUSED', good + [clr_ok], 7, False),
        ('F10 v7 payload-less ClearF accepted', good + [clr_none], 7, True),
        ('F10 v8 cube upload (base+mip) accepted', good + [cube_ok], 8, True),
    ]
    for i, (name, recs, ver, want_ok) in enumerate(cases):
        pv = os.path.join(tmp, 'f9_%02d.srht' % i)
        open(pv, 'wb').write(_trace(recs, version=ver))
        try:
            load(pv, quiet=True)
            check(name, want_ok, 'load() ACCEPTED it')
        except TraceError as e:
            check(name, not want_ok, str(e))

    # F5 — a well-formed trace must VALIDATE.
    p5 = os.path.join(tmp, 'f5_good.srht')
    open(p5, 'wb').write(_trace(good))
    print('\nF5 well-formed trace')
    try:
        load(p5, quiet=True)
        check('F5 accepted', True)
    except TraceError as e:
        check('F5 accepted', False, str(e))

    print(f'\n{"SELFTEST PASSED" if not failures else "SELFTEST FAILED"}: '
          f'{len(ran) - len(failures)}/{len(ran)} assertions passed, '
          f'{len(failures)} failure(s)')
    for f in failures:
        print(f'  - {f}')
    if not a.keep:
        print(f'(fixtures left in {tmp})')
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('verify', help='structural validation only')
    p.add_argument('trace')
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser('resolves', help='ordered Resolve census + per-destination')
    p.add_argument('trace')
    p.add_argument('--log', help='run log supplying the runtime success counts')
    p.add_argument('--max-binds', type=int, default=40)
    p.set_defaults(fn=cmd_resolves)

    p = sub.add_parser('writers', help='last ELIGIBLE writer of every destination')
    p.add_argument('trace')
    p.add_argument('--tail', type=int, default=4)
    p.set_defaults(fn=cmd_writers)

    p = sub.add_parser('window', help='dump a record range')
    p.add_argument('trace')
    p.add_argument('fromRec', type=int)
    p.add_argument('toRec', type=int)
    p.add_argument('-v', '--verbose', action='store_true')
    p.set_defaults(fn=cmd_window)

    p = sub.add_parser('draw', help='full contract audit of one draw')
    p.add_argument('trace')
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--rec', type=int, help='trace RECORD index of the draw')
    g.add_argument('--shader', help='VSHEX:PSHEX identity to match')
    p.add_argument('--all', action='store_true',
                   help='report every match, not just the first')
    p.set_defaults(fn=cmd_draw)

    p = sub.add_parser('selftest', help='adversarial fixtures + assertions')
    p.add_argument('--keep', help='write fixtures to this dir instead of a temp one')
    p.set_defaults(fn=cmd_selftest)

    a = ap.parse_args()
    try:
        return a.fn(a) or 0
    except TraceError as e:
        print(f'\nREFUSED: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
