"""srht_texkey.py — inspect / rewrite a texture key's payload inside a .srht trace.

GENERAL by construction: trace path, texture key and replacement source are all
arguments. Nothing about any particular key, glyph, string or shader is encoded.

  inspect : python tools/srht_texkey.py inspect <trace> <hexkey> [--dump DIR]
            Enumerate every TextureUpload for the key in trace order (version 0,
            1, 2 ... = order of appearance), report payload stats + digests, and
            optionally BANK each raw payload to DIR so later byte-identity
            claims can use direct comparison instead of digest equality.

  rewrite : python tools/srht_texkey.py rewrite <inTrace> <outTrace> <hexkey> \
                   --from-file PAYLOAD [--version N]
            Replace ONLY the payload bytes of that key's TextureUpload(s) with
            PAYLOAD (which must be the same size), regenerating the record's
            payloadHash with XXH3-64 so the replayer reports hashMismatches=0.
            Every other byte of the trace is copied verbatim.

Trace layout (gfx/hle_video.{hpp,cpp}):
  header 32B : magic[4]"SRHT", u32 version, u64 records, u64 payloadBytes,
               u32 presents, u32 reserved
  record 48B : RenderCommand (u32 type + 7 u32), u32 payloadBytes,
               u32 reserved, u64 payloadHash,  then payloadBytes of payload
  TextureUpload union: key, arenaOffset, bytes, dims, fmtBits, pitch
payloadHash = XXH3_64bits(payload) — gfx/shader_cache.cpp:107.
"""
import argparse, os, struct, sys

try:
    import xxhash
except ImportError:
    sys.exit('FATAL: pip install xxhash (the trace hash is XXH3-64)')

T_TEXUP = 18
HDR = struct.Struct('<4sIQQII')
REC = struct.Struct('<8I')
TAIL = struct.Struct('<IIQ')


def walk(data):
    """Yield (index, off, cmd tuple, payloadBytes, payloadHash, bodyOff)."""
    off, idx = 32, 0
    while off + 48 <= len(data):
        cmd = REC.unpack_from(data, off)
        pbytes, _res, phash = TAIL.unpack_from(data, off + 32)
        body = off + 48
        if body + pbytes > len(data):
            raise ValueError(f'record {idx} payload overruns EOF')
        yield idx, off, cmd, pbytes, phash, body
        off = body + pbytes
        idx += 1
    if off != len(data):
        raise ValueError(f'trailing {len(data)-off} bytes after last record')


def stats(pay):
    nz = sum(1 for b in pay if b)
    first = next((i for i, b in enumerate(pay) if b), -1)
    last = -1
    for i in range(len(pay) - 1, -1, -1):
        if pay[i]:
            last = i
            break
    return nz, first, last


def cmd_inspect(a):
    data = open(a.trace, 'rb').read()
    magic, ver, nrec, npay, presents, _ = HDR.unpack_from(data, 0)
    print(f'trace   : {a.trace}')
    print(f'header  : magic={magic!r} version={ver} records={nrec} '
          f'payloadBytes={npay} presents={presents} fileBytes={len(data)}')
    if magic != b'SRHT':
        sys.exit('FATAL: bad magic')
    want = int(a.key, 16) & 0x1FFFFFFF
    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
    n = 0
    last_idx = 0
    for idx, _off, cmd, pbytes, phash, body in walk(data):
        last_idx = idx
        if cmd[0] != T_TEXUP or (cmd[1] & 0x1FFFFFFF) != want:
            continue
        pay = data[body:body + pbytes]
        nz, first, last = stats(pay)
        w, h = cmd[4] >> 16, cmd[4] & 0xFFFF
        print(f'\n--- version {n} (record {idx}) ---')
        print(f'  key={cmd[1]:08X} bytes={cmd[3]} payload={pbytes} {w}x{h} '
              f'fmt={cmd[5] & 0xFF} tiled={(cmd[5] >> 8) & 1} '
              f'endian={(cmd[5] >> 12) & 3} pitch={cmd[6]}')
        print(f'  nonzero={nz} first={first} last={last}')
        print(f'  xxh3_64(recorded)=0x{phash:016X}  '
              f'xxh3_64(actual)=0x{xxhash.xxh3_64_intdigest(pay):016X}  '
              f'{"OK" if phash == xxhash.xxh3_64_intdigest(pay) else "MISMATCH"}')
        if a.dump:
            p = os.path.join(a.dump, f'{want:08X}_v{n}_{pbytes}B.bin')
            open(p, 'wb').write(pay)
            print(f'  banked raw payload -> {p}')
        n += 1
    print(f'\nwalked {last_idx + 1} records, CLEAN EOF')
    print(f'TextureUpload records for {want:08X}: {n}')


def cmd_rewrite(a):
    data = bytearray(open(a.intrace, 'rb').read())
    magic = HDR.unpack_from(data, 0)[0]
    if magic != b'SRHT':
        sys.exit('FATAL: bad magic')
    want = int(a.key, 16) & 0x1FFFFFFF
    repl = open(a.from_file, 'rb').read()
    hits, n = 0, 0
    for idx, off, cmd, pbytes, phash, body in list(walk(bytes(data))):
        if cmd[0] != T_TEXUP or (cmd[1] & 0x1FFFFFFF) != want:
            continue
        if a.version is not None and n != a.version:
            n += 1
            continue
        n += 1
        if pbytes != len(repl):
            sys.exit(f'FATAL: record {idx} payload is {pbytes} B but the '
                     f'replacement is {len(repl)} B — refusing to resize '
                     f'(that would change every later record offset)')
        data[body:body + pbytes] = repl
        newh = xxhash.xxh3_64_intdigest(repl)
        struct.pack_into('<Q', data, off + 40, newh)   # payloadHash field
        print(f'record {idx}: replaced {pbytes} B, payloadHash '
              f'0x{phash:016X} -> 0x{newh:016X}')
        hits += 1
    if not hits:
        sys.exit('FATAL: no matching TextureUpload record was rewritten')
    open(a.outtrace, 'wb').write(bytes(data))
    print(f'wrote {a.outtrace} ({len(data)} bytes, {hits} record(s) rewritten)')
    # Re-walk the OUTPUT to prove it is still structurally valid.
    out = open(a.outtrace, 'rb').read()
    cnt = sum(1 for _ in walk(out))
    print(f'verify: output walks {cnt} records to CLEAN EOF')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('inspect')
    i.add_argument('trace'); i.add_argument('key')
    i.add_argument('--dump', default=None)
    i.set_defaults(fn=cmd_inspect)
    r = sub.add_parser('rewrite')
    r.add_argument('intrace'); r.add_argument('outtrace'); r.add_argument('key')
    r.add_argument('--from-file', dest='from_file', required=True)
    r.add_argument('--version', type=int, default=None,
                   help='rewrite only the Nth (0-based) upload for the key')
    r.set_defaults(fn=cmd_rewrite)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
