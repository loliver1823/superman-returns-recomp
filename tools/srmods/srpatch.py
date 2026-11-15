#!/usr/bin/env python3
"""srpatch.py - the SRPATCH1 delta format of port features and patch-style mods (docs/MODS.md
section 7.2): a patch turns ONE base (a container entry of the player's own disc) into one
output using copy instructions plus our own bytes, so the repo and the release carry no
game data.

Format (all integers little-endian; varints are LEB128, signed ones zigzag):
  0x00  8s   b"SRPATCH1"
  0x08  u32  format (1)
  0x0C  u32  0
  0x10  u64  base size
  0x18  32s  base sha256 (raw digest)
  0x38  u64  output size
  0x40  32s  output sha256 (raw digest)
  0x60  ops, then END:
        0x01 COPY   svarint(baseOff - cursor) uvarint(len)   from the base; cursor = baseOff + len
        0x02 ADD    uvarint(len) bytes[len]                    our bytes
        0x03 FILL   u8 value uvarint(len)                       a run of one byte value
        0x04 COPYF  svarint(fileOff - fcursor) uvarint(len)   from the base's whole disc FILE
                                                                (the top-level container the
                                                                entry lives in); fcursor likewise
        0x00 END    (must be the last byte of the file)
  cursor and fcursor start at 0. Every op has len >= 1; a COPY stays inside the base; the ops
  produce exactly `output size` bytes, whose SHA-256 must match (so a COPYF from a different
  file can never go unnoticed). The exe's applier (kernel/mod_formats.cpp srpatchApply) and
  apply() below implement the same rules and both refuse anything else.

Cleanliness rule (dirty_windows): no ADD run may contain a window of WINDOW (32) bytes that
also occurs anywhere in the base FILE. The encoder makes that hold by construction: every
such window becomes a COPY (inside the entry) or a COPYF (elsewhere in the file) - a hint
loop - and package_port.py refuses a patch that fails it.

usage:
  python tools/mods/srpatch.py encode BASE OUT PATCH [--file FILE]   # write PATCH (prints the stats)
  python tools/mods/srpatch.py apply  BASE PATCH OUT [--file FILE]   # rebuild OUT, verifying both hashes
  python tools/mods/srpatch.py check  BASE PATCH [--file FILE]       # decode + cleanliness report
"""
import hashlib
import struct
import sys

MAGIC = b"SRPATCH1"
FORMAT = 1
HDR = struct.Struct("<8sII Q32s Q32s")
assert HDR.size == 0x60
OP_END, OP_COPY, OP_ADD, OP_FILL, OP_COPYF = 0, 1, 2, 3, 4

WINDOW = 32            # the cleanliness window (bytes)
MIN_CONT = 4           # a continuation copy (same or substitution-shifted base position)
MIN_FIND = 8           # a copy found by searching the base near the cursor
SEARCH = 1 << 16       # +- bytes searched around the cursor
MIN_FILL = 8           # a run of one byte value this long becomes FILL


class PatchError(Exception):
    pass


# ------------------------------------------------------------------ varints
def uvarint(n):
    assert n >= 0
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def svarint(n):
    return uvarint((n << 1) if n >= 0 else ((-n << 1) - 1))


def read_uvarint(buf, p):
    n = shift = 0
    for k in range(10):
        if p >= len(buf):
            raise PatchError("truncated varint")
        b = buf[p]
        p += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if n >= 1 << 64:
                raise PatchError("varint overflows 64 bits")
            return n, p
    raise PatchError("varint longer than 10 bytes")


def read_svarint(buf, p):
    z, p = read_uvarint(buf, p)
    return ((z >> 1) if not z & 1 else -((z + 1) >> 1)), p


def sha256(b):
    return hashlib.sha256(b).digest()


# ------------------------------------------------------------------ matching
def match_len(a, i, b, p):
    """length of the common prefix of a[i:] and b[p:] (chunked, C-speed compares)."""
    n = min(len(a) - i, len(b) - p)
    if n <= 0:
        return 0
    L, step = 0, 64
    while L < n:
        s = min(step, n - L)
        if a[i + L:i + L + s] == b[p + L:p + L + s]:
            L += s
            step = min(step * 2, 1 << 20)
            continue
        lo, hi = 0, s          # first mismatch lies in [L+lo, L+hi)
        while hi - lo > 8:
            mid = (lo + hi) // 2
            if a[i + L + lo:i + L + mid] == b[p + L + lo:p + L + mid]:
                lo = mid
            else:
                hi = mid
        for k in range(lo, hi):
            if a[i + L + k] != b[p + L + k]:
                return L + k
        return L + hi  # unreachable in practice
    return L


def _flush_literal(ops, lit):
    """append ADD/FILL ops for the literal bytes `lit` (FILL for runs >= MIN_FILL)."""
    i, n = 0, len(lit)
    start = 0
    while i < n:
        j = i + 1
        while j < n and lit[j] == lit[i]:
            j += 1
        if j - i >= MIN_FILL:
            if start < i:
                ops.append((OP_ADD, bytes(lit[start:i])))
            ops.append((OP_FILL, lit[i], j - i))
            start = j
        i = j
    if start < n:
        ops.append((OP_ADD, bytes(lit[start:n])))


def encode_ops(base, out, hints=None, file=None, fhints=None):
    """-> list of ops: (OP_COPY, baseOff, len) | (OP_ADD, bytes) | (OP_FILL, value, len) |
    (OP_COPYF, fileOff, len). hints {outPos: basePos} / fhints {outPos: filePos}: windows the
    cleanliness check found (forced candidates)."""
    hints = hints or {}
    fhints = fhints or {}
    ops = []
    lit = bytearray()
    n, nb = len(out), len(base)
    i = 0
    cursor = 0     # base position after the last COPY (an insertion keeps it)
    pred = 0       # base position predicted for out[i] if literal bytes REPLACED base bytes

    def consider(best, p, minimum, src=0):
        buf = base if src == 0 else file
        if p is None or p < 0 or p >= len(buf) or out[i] != buf[p]:
            return best
        L = match_len(out, i, buf, p)
        if L >= minimum and (best is None or L > best[1]):
            return (p, L, src)
        return best

    while i < n:
        best = None
        best = consider(best, cursor, MIN_CONT)
        if pred != cursor:
            best = consider(best, pred, MIN_CONT)
        if i in hints:
            best = consider(best, hints[i], MIN_FIND)
        if (best is None or best[1] < WINDOW) and i + MIN_FIND <= n:
            key = out[i:i + MIN_FIND]
            f = base.find(key, max(0, cursor), min(nb, cursor + SEARCH))
            if f >= 0:
                best = consider(best, f, MIN_FIND)
            r = base.rfind(key, max(0, cursor - SEARCH), min(nb, cursor + MIN_FIND))
            if r >= 0 and r != f:
                best = consider(best, r, MIN_FIND)
        if i in fhints and file is not None:
            best = consider(best, fhints[i], WINDOW, 1)
        if best is None:
            lit.append(out[i])
            i += 1
            pred += 1
            continue
        p, L, src = best
        buf = base if src == 0 else file
        # extend backwards over pending literal bytes the source also has
        back = 0
        while back < len(lit) and p - back - 1 >= 0 and buf[p - back - 1] == lit[len(lit) - back - 1]:
            back += 1
        if back:
            del lit[len(lit) - back:]
            p -= back
            L += back
        if lit:
            _flush_literal(ops, lit)
            lit = bytearray()
        kind = OP_COPY if src == 0 else OP_COPYF
        if ops and ops[-1][0] == kind and ops[-1][1] + ops[-1][2] == p:
            ops[-1] = (kind, ops[-1][1], ops[-1][2] + L)
        else:
            ops.append((kind, p, L))
        i += L - back
        if src == 0:
            cursor = pred = p + L
        else:
            pred += L
    if lit:
        _flush_literal(ops, lit)
    return ops


def serialize(ops, base, out):
    buf = bytearray(HDR.pack(MAGIC, FORMAT, 0, len(base), sha256(base), len(out), sha256(out)))
    cursor = fcursor = 0
    for op in ops:
        if op[0] in (OP_COPY, OP_COPYF):
            _, p, L = op
            buf.append(op[0])
            buf += svarint(p - (cursor if op[0] == OP_COPY else fcursor))
            buf += uvarint(L)
            if op[0] == OP_COPY:
                cursor = p + L
            else:
                fcursor = p + L
        elif op[0] == OP_ADD:
            buf.append(OP_ADD)
            buf += uvarint(len(op[1]))
            buf += op[1]
        elif op[0] == OP_FILL:
            buf.append(OP_FILL)
            buf.append(op[1])
            buf += uvarint(op[2])
        else:
            raise PatchError(f"bad op {op[0]}")
    buf.append(OP_END)
    return bytes(buf)


def parse(patch):
    """-> (header dict, ops). Raises PatchError on anything malformed (same rules as the exe).
    COPYF ranges are checked against the file only when applied."""
    if len(patch) < HDR.size + 1:
        raise PatchError("patch shorter than its header")
    magic, fmt, rsv, bsize, bsha, osize, osha = HDR.unpack_from(patch, 0)
    if magic != MAGIC:
        raise PatchError("not an SRPATCH1 file")
    if fmt != FORMAT or rsv != 0:
        raise PatchError(f"unsupported format {fmt}/{rsv}")
    hdr = {"base_size": bsize, "base_sha256": bsha, "out_size": osize, "out_sha256": osha,
           "uses_file": False}
    ops = []
    p, cursor, fcursor, produced = HDR.size, 0, 0, 0
    while True:
        if p >= len(patch):
            raise PatchError("missing END")
        k = patch[p]
        p += 1
        if k == OP_END:
            if p != len(patch):
                raise PatchError("bytes after END")
            break
        if k in (OP_COPY, OP_COPYF):
            d, p = read_svarint(patch, p)
            L, p = read_uvarint(patch, p)
            off = (cursor if k == OP_COPY else fcursor) + d
            if L == 0 or off < 0:
                raise PatchError("empty or negative copy")
            if k == OP_COPY:
                if off + L > bsize:
                    raise PatchError(f"COPY {off:#x}+{L} outside the base ({bsize} B)")
                cursor = off + L
            else:
                fcursor = off + L
                hdr["uses_file"] = True
            ops.append((k, off, L))
        elif k == OP_ADD:
            L, p = read_uvarint(patch, p)
            if L == 0 or p + L > len(patch):
                raise PatchError("ADD runs past the patch end")
            ops.append((OP_ADD, bytes(patch[p:p + L])))
            p += L
        elif k == OP_FILL:
            if p >= len(patch):
                raise PatchError("truncated FILL")
            v = patch[p]
            p += 1
            L, p = read_uvarint(patch, p)
            if L == 0:
                raise PatchError("empty FILL")
            ops.append((OP_FILL, v, L))
        else:
            raise PatchError(f"unknown op {k:#x} at {p - 1:#x}")
        produced += ops[-1][2] if ops[-1][0] != OP_ADD else len(ops[-1][1])
        if produced > osize:
            raise PatchError("ops produce more than the output size")
    if produced != osize:
        raise PatchError(f"ops produce {produced} B, header says {osize}")
    return hdr, ops


def apply_ops(base, ops, file=None):
    """file: bytes-like or a callable (off, n) -> bytes (the base's disc file)."""
    out = bytearray()
    for op in ops:
        if op[0] == OP_COPY:
            out += base[op[1]:op[1] + op[2]]
        elif op[0] == OP_COPYF:
            if file is None:
                raise PatchError("the patch copies from the base file, which is not available")
            got = file(op[1], op[2]) if callable(file) else file[op[1]:op[1] + op[2]]
            if len(got) != op[2]:
                raise PatchError(f"COPYF {op[1]:#x}+{op[2]} outside the base file")
            out += got
        elif op[0] == OP_ADD:
            out += op[1]
        else:
            out += bytes([op[1]]) * op[2]
    return bytes(out)


def apply(base, patch, file=None):
    """the reference applier: verify the base, decode, verify the output. Raises PatchError."""
    hdr, ops = parse(patch)
    if len(base) != hdr["base_size"] or sha256(base) != hdr["base_sha256"]:
        raise PatchError("the base is not the one this patch was made for")
    out = apply_ops(base, ops, file)
    if len(out) != hdr["out_size"] or sha256(out) != hdr["out_sha256"]:
        raise PatchError("the output does not match the patch's output hash")
    return out


# ------------------------------------------------------------------ cleanliness
def add_runs(ops):
    """-> [(outPos, bytes)] maximal runs of ADD bytes (adjacent ADD ops merged)."""
    runs, pos, cur, cur_at = [], 0, None, 0
    for op in ops:
        if op[0] == OP_ADD:
            if cur is None:
                cur, cur_at = bytearray(), pos
            cur += op[1]
            pos += len(op[1])
            continue
        if cur is not None:
            runs.append((cur_at, bytes(cur)))
            cur = None
        pos += op[2]
    if cur is not None:
        runs.append((cur_at, bytes(cur)))
    return runs


def dirty_windows(base, runs, window=WINDOW, limit=1 << 30):
    """every WINDOW-byte window of an ADD run that also occurs in `base`, as
    [(outPos, basePos)]. Complete: a base occurrence at p contains the aligned 16-gram at
    q = ceil(p/16)*16 (q - p <= 15 and q + 16 <= p + 32), so scanning the base at stride 16
    against every 16-gram of the runs finds it; each candidate is then compared in full."""
    g = window // 2
    assert window >= 2 * g and g >= 1
    grams = {}
    for ri, (_, data) in enumerate(runs):
        if len(data) < window:
            continue
        for k in range(len(data) - g + 1):
            grams.setdefault(data[k:k + g], []).append((ri, k))
    if not grams:
        return []
    found, seen = [], set()
    nb = len(base)
    for q in range(0, nb - g + 1, g):
        hits = grams.get(base[q:q + g])
        if not hits:
            continue
        for ri, k in hits:
            at, data = runs[ri]
            for d in range(g):
                s, p = k - d, q - d
                if s < 0 or p < 0 or s + window > len(data) or p + window > nb:
                    continue
                if (at + s) in seen:
                    continue
                if data[s:s + window] == base[p:p + window]:
                    seen.add(at + s)
                    found.append((at + s, p))
                    if len(found) >= limit:
                        return found
    return found


def encode(base, out, log=None, max_rounds=12, file=None):
    """-> (patch bytes, stats). The hint loop: encode, find every ADD window that occurs in the
    base (or, with `file`, anywhere in the base's disc file), give those positions to the
    encoder as forced candidates (COPY inside the base, COPYF from the file), repeat until
    clean. Raises PatchError if it does not converge."""
    hints, fhints = {}, {}
    for rnd in range(max_rounds):
        ops = encode_ops(base, out, hints, file, fhints)
        if apply_ops(base, ops, file) != out:
            raise PatchError("encoder self-check: the ops do not rebuild the output")
        runs = add_runs(ops)
        dirty = dirty_windows(base, runs)
        fdirty = dirty_windows(file, runs) if file is not None else []
        if log:
            log(f"  round {rnd + 1}: {len(ops)} ops, {sum(len(o[1]) for o in ops if o[0] == OP_ADD)} ADD B, "
                f"{len(dirty)}+{len(fdirty)} dirty window(s)")
        if not dirty and not fdirty:
            patch = serialize(ops, base, out)
            return patch, stats(base, patch)
        new = {o: p for o, p in dirty if o not in hints}
        fnew = {o: p for o, p in fdirty if o not in fhints}
        if not new and not fnew:
            first = (dirty or fdirty)[0]
            raise PatchError(f"cleanliness: {len(dirty) + len(fdirty)} ADD window(s) occur in the base and the "
                             f"encoder cannot turn them into copies (first at output {first[0]:#x})")
        hints.update(new)
        fhints.update(fnew)
    raise PatchError("cleanliness: the hint loop did not converge")


def stats(base, patch):
    hdr, ops = parse(patch)
    runs = add_runs(ops)
    add_bytes = sum(len(d) for _, d in runs)
    return {
        "patch_size": len(patch),
        "ops": len(ops),
        "copy_ops": sum(1 for o in ops if o[0] == OP_COPY),
        "copy_bytes": sum(o[2] for o in ops if o[0] == OP_COPY),
        "copyf_ops": sum(1 for o in ops if o[0] == OP_COPYF),
        "copyf_bytes": sum(o[2] for o in ops if o[0] == OP_COPYF),
        "add_runs": len(runs),
        "add_bytes": add_bytes,
        "max_add_run": max((len(d) for _, d in runs), default=0),
        "fill_bytes": sum(o[2] for o in ops if o[0] == OP_FILL),
        "out_size": hdr["out_size"],
    }


def clean_check(base, patch, file=None):
    """-> (ok, stats, dirty, out). ok = the patch decodes against this base AND no ADD window
    of WINDOW bytes occurs in the base (or in `file`, the base's disc file, when given)."""
    out = apply(base, patch, file)
    _, ops = parse(patch)
    runs = add_runs(ops)
    dirty = dirty_windows(base, runs) + (dirty_windows(file, runs) if file is not None else [])
    return (not dirty), stats(base, patch), dirty, out


# ------------------------------------------------------------------ CLI
def _main(argv):
    fpath = None
    if "--file" in argv:
        i = argv.index("--file")
        fpath = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) < 3 or argv[0] not in ("encode", "apply", "check"):
        print(__doc__)
        return 2
    cmd = argv[0]
    rd = lambda p: open(p, "rb").read()
    file = rd(fpath) if fpath else None
    try:
        if cmd == "encode":
            base, out = rd(argv[1]), rd(argv[2])
            patch, st = encode(base, out, log=print, file=file)
            open(argv[3], "wb").write(patch)
            print(st)
        elif cmd == "apply":
            out = apply(rd(argv[1]), rd(argv[2]), file)
            open(argv[3], "wb").write(out)
            print(f"wrote {argv[3]} ({len(out)} B, sha256 {hashlib.sha256(out).hexdigest()})")
        else:
            ok, st, dirty, _ = clean_check(rd(argv[1]), rd(argv[2]), file)
            print(st)
            print("CLEAN" if ok else f"DIRTY: {len(dirty)} window(s), first {dirty[:5]}")
            return 0 if ok else 1
    except PatchError as ex:
        print(f"srpatch: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
