#!/usr/bin/env python3
"""
golden.py — direct, fast access to the GOLDEN Xenia dumps (data/xenia_canary*.DMP).

Each dump is a full-memory Windows minidump of xenia_canary.exe. Select one with
--dump NAME (or env GOLDEN_DUMP=NAME, or a path); the default stays the original:

  nosound            data/xenia_canary.DMP (2026-06-13, "NO SOUND" ISO, press-start
                     menu, older stock Xenia build) - the historical golden.
  sound_menu         data/xenia_canary_sound_menu.DMP (iter-463, FULL sound ISO,
                     oracle build canary_experimental@0bd090dbe, xma_decoder="new",
                     use_dedicated_xma_thread=true, press-start screen).
  sound_gameplay     data/xenia_canary_sound_gameplay.DMP (same build/ISO, Metropolis
                     gameplay with HUD after the opening cinematic).

Xenia maps the 4GB guest space at host base 0x100000000, big-endian, intact —
so guest VA X lives at host 0x100000000 + X, EXCEPT the 0xE0000000-0xFFFFFFFF
physical view: that heap has host_address_offset 0x1000 on Windows (Xenia
memory.cc), so VA X >= 0xE0000000 lives at host 0x100000000 + X + 0x1000.
read()/find() apply that offset (before iter-463 they did not, so E-range reads
returned the bytes of X - 0x1000). This module parses the minidump's
Memory64ListStream (pure stdlib, no cdb/WinDbg, no 30s MCP timeouts) and gives
read / find / diff over guest memory.

Proven uses (iter-295): locate MAIN's .apt blob in golden memory, diff it against
the raw file to reveal relocated pointer fields (the registrar relocates in
place), find runtime holders/TOCs by searching for known guest VAs (write the
VA big-endian). See docs/ENGINE_MAP.md §8 "Golden-dump differential".

CLI (any command accepts --dump NAME|PATH first):
  python tools/golden.py --dump sound_gameplay read 0x829AC104 4
  python tools/golden.py ranges
  python tools/golden.py read  <guestVA> <size>              # hexdump
  python tools/golden.py find  ascii:__Packages.PartHighlightBar [startVA endVA]
  python tools/golden.py find  hex:4170742044617461 [startVA endVA]
  python tools/golden.py findva <guestVA> [startVA endVA]    # find BE pointer refs
  python tools/golden.py diff  <file> <guestVA>              # dword diff vs golden
"""
import mmap
import os
import struct
import sys

_DATA = os.path.join(os.path.dirname(__file__), "..", "data")
DUMPS = {
    "nosound": os.path.join(_DATA, "xenia_canary.DMP"),
    "sound_menu": os.path.join(_DATA, "xenia_canary_sound_menu.DMP"),
    "sound_gameplay": os.path.join(_DATA, "xenia_canary_sound_gameplay.DMP"),
}
DUMP = DUMPS["nosound"]
GUEST_HOST_BASE = 0x100000000            # xenia guest VA 0 lives here
E_VIEW_BASE = 0xE0000000                 # vE0000000 heap: host_address_offset 0x1000
E_VIEW_HOST_OFFSET = 0x1000


def resolve_dump(name_or_path=None):
    """Map a dump name (DUMPS key) or a path to a path; None -> env GOLDEN_DUMP or default."""
    if name_or_path is None:
        name_or_path = os.environ.get("GOLDEN_DUMP") or "nosound"
    return DUMPS.get(name_or_path, name_or_path)


def guest_to_host(guest_va):
    guest_va &= 0xFFFFFFFF
    extra = E_VIEW_HOST_OFFSET if guest_va >= E_VIEW_BASE else 0
    return GUEST_HOST_BASE + guest_va + extra


def host_to_guest(host):
    off = host - GUEST_HOST_BASE
    if off >= E_VIEW_BASE + E_VIEW_HOST_OFFSET:
        off -= E_VIEW_HOST_OFFSET
    return off

MEMORY64_LIST_STREAM = 9


class GoldenDump:
    def __init__(self, path=None):
        path = resolve_dump(path)
        self.path = path
        self._f = open(path, "rb")
        self._m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        sig, _ver, nstreams, dir_rva = struct.unpack_from("<4sIII", self._m, 0)
        if sig != b"MDMP":
            raise ValueError("not a minidump")
        self.ranges = []                 # (host_start, size, file_off), sorted
        for i in range(nstreams):
            stype, _dsize, drva = struct.unpack_from("<III", self._m, dir_rva + i * 12)
            if stype == MEMORY64_LIST_STREAM:
                nranges, base_rva = struct.unpack_from("<QQ", self._m, drva)
                off = base_rva
                for j in range(nranges):
                    start, size = struct.unpack_from("<QQ", self._m, drva + 16 + j * 16)
                    self.ranges.append((start, size, off))
                    off += size
                break
        if not self.ranges:
            raise ValueError("no Memory64ListStream (not a full-memory dump?)")
        self.ranges.sort()

    def read_host(self, addr, size):
        """Read host-address bytes; returns b'' for unmapped."""
        out = bytearray()
        need = size
        pos = addr
        for start, rsize, foff in self.ranges:
            if pos >= start + rsize or pos + need <= start:
                continue
            take_from = max(pos, start)
            take = min(need - (take_from - pos), start + rsize - take_from)
            if take_from > pos:          # gap before this range
                return bytes(out)
            out += self._m[foff + (take_from - start): foff + (take_from - start) + take]
            pos += take
            need -= take
            if need <= 0:
                break
        return bytes(out)

    def read(self, guest_va, size):
        return self.read_host(guest_to_host(guest_va), size)

    def u32be(self, guest_va):
        b = self.read(guest_va, 4)
        return struct.unpack(">I", b)[0] if len(b) == 4 else None

    def find(self, pattern, start_va=0, end_va=0x100000000, limit=32):
        """Search guest memory for raw bytes; returns guest VAs."""
        hits = []
        lo = guest_to_host(start_va) if start_va < 0x100000000 else GUEST_HOST_BASE + start_va
        hi = guest_to_host(end_va) if end_va < 0x100000000 else GUEST_HOST_BASE + end_va + E_VIEW_HOST_OFFSET
        for start, rsize, foff in self.ranges:
            if start + rsize <= lo or start >= hi:
                continue
            a = max(start, lo)
            b = min(start + rsize, hi)
            chunk_off = foff + (a - start)
            data = self._m[chunk_off: chunk_off + (b - a)]
            i = data.find(pattern)
            while i != -1:
                hits.append(host_to_guest(a + i))
                if len(hits) >= limit:
                    return hits
                i = data.find(pattern, i + 1)
        return hits

    def find_va_refs(self, guest_va, start_va=0, end_va=0x100000000, limit=32):
        """Find big-endian pointers TO guest_va (how holders/TOCs are found)."""
        return self.find(struct.pack(">I", guest_va & 0xFFFFFFFF), start_va, end_va, limit)

    def diff_file(self, file_path, guest_va):
        """Dword-diff a raw file against golden memory at guest_va.
        Returns list of (offset, file_u32be, golden_u32be). The classic read:
        golden == file + blobBase  =>  blob-base-relocated pointer field."""
        data = open(file_path, "rb").read()
        gold = self.read(guest_va, len(data))
        out = []
        for o in range(0, min(len(data), len(gold)) - 3, 4):
            fv = struct.unpack_from(">I", data, o)[0]
            gv = struct.unpack_from(">I", gold, o)[0]
            if fv != gv:
                out.append((o, fv, gv))
        return out


def _hexdump(va, data):
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in row)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print(f"{va + i:08x}  {hexs:<47}  {asc}")


def main(argv):
    dump = None
    if len(argv) > 2 and argv[1] == "--dump":
        dump = argv[2]
        argv = argv[:1] + argv[3:]
    if len(argv) < 2:
        print(__doc__)
        return 1
    g = GoldenDump(dump)
    cmd = argv[1]
    if cmd == "ranges":
        print(f"{len(g.ranges)} ranges; guest window examples:")
        for start, size, _ in g.ranges[:20]:
            if start >= GUEST_HOST_BASE and start < GUEST_HOST_BASE + 0x100000000:
                print(f"  guest {host_to_guest(start):08x} +{size:x}")
    elif cmd == "read":
        va, size = int(argv[2], 0), int(argv[3], 0)
        _hexdump(va, g.read(va, size))
    elif cmd == "find":
        pat = argv[2]
        if pat.startswith("ascii:"):
            needle = pat[6:].encode()
        elif pat.startswith("hex:"):
            needle = bytes.fromhex(pat[4:])
        else:
            needle = pat.encode()
        lo = int(argv[3], 0) if len(argv) > 3 else 0
        hi = int(argv[4], 0) if len(argv) > 4 else 0x100000000
        for va in g.find(needle, lo, hi):
            print(f"0x{va:08x}")
    elif cmd == "findva":
        target = int(argv[2], 0)
        lo = int(argv[3], 0) if len(argv) > 3 else 0
        hi = int(argv[4], 0) if len(argv) > 4 else 0x100000000
        for va in g.find_va_refs(target, lo, hi):
            print(f"0x{va:08x}")
    elif cmd == "diff":
        path, va = argv[2], int(argv[3], 0)
        diffs = g.diff_file(path, va)
        print(f"{len(diffs)} differing dwords (offset: file -> golden; delta):")
        for o, fv, gv in diffs[:200]:
            print(f"  +0x{o:04x}: {fv:08x} -> {gv:08x}  (d=0x{(gv - fv) & 0xFFFFFFFF:x})")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
