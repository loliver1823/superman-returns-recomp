#!/usr/bin/env python3
"""guest_snap.py - the `.snap` guest-memory image: only the guest pages a reader touched.

WHY (docs/DEVTOOLS_DESIGN.md 6.2 step 5): the level-skip verification snapshots the same
guest state in Xenia and in our build at each settle point, then runs the SAME readers
(tools/objstate.py, tools/save_records.py) over both. A full dump per settle point is
gigabytes; the readers touch ~170 pages. A `.snap` stores exactly those pages, and the
readers open it through the shared image layer (tools/havok_snapshot.py open_image) as
`snap:<file>`, next to `xenia:` and `ours:`. The ranges must be found in each snapshot's own
run (heap addresses move), so a snap is always written from the reader's own read ledger on
the image being captured (`objstate.py snap --write-snap`), never from a reused ledger.

Rule: a read outside the captured pages is SHORT, never zero-filled, so the strict readers
turn it into a failed check (objstate Mem raises ReadError) instead of a plausible zero.

File format (all integers little-endian; the page bytes are raw guest memory, big-endian
data untouched):
  +0   8 bytes  magic b"SRSNAP\\x00\\x01"
  +8   u32      meta length L
  +12  L bytes  meta, UTF-8 JSON: {"schema": "guest_snap/1", "source": <image describe()>,
                "label", "reader", "created", "missing": [unreadable page VAs], ...}
  then u32      page count N
  then N x {u32 page number (VA >> 12), u32 valid length 1..4096}, strictly increasing pages
  then the page bytes, each page's valid length, in table order (no padding)
  then u32      CRC-32 (zlib) of every preceding byte
A partial page (valid length < 4096) is what the source image itself returned for that
page (a host-window or dump-range edge): reads stop there exactly as they did on the source.
The E0000000 view's +0x1000 host offset is applied by the SOURCE image (golden.py /
havok_snapshot XeniaImage); a snap holds guest VAs only.

Usage:
  python tools/guest_snap.py info FILE.snap
  python tools/guest_snap.py capture --image SPEC --out FILE.snap [--readers objstate,save_records]
    runs the readers over SPEC through a page-recording wrapper, writes the union of the pages
    they requested, then re-runs them on the snap and exits 1 unless every output is identical
    and no read fell outside the snap (the self-check).
  python tools/guest_snap.py capture --image SPEC --ledger LEDGER.json --out FILE.snap
    (LEDGER = the JSON [[start,end],...] of objstate.py snap --ledger; objstate-only one-step
     form: python tools/objstate.py snap --image SPEC --write-snap FILE.snap)
Tests: tools/test_guest_snap.py.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
MAGIC = b"SRSNAP\x00\x01"
SCHEMA = "guest_snap/1"
PAGE = 0x1000
MAX_PAGES = 1 << 20          # the whole 32-bit guest space

# ADAPTER POINT: the image base class is tools/havok_snapshot.py's (u8/u32/scan_u32 helpers),
# so a SnapImage works for every reader of that layer; a minimal base keeps the format usable
# when that module is absent.
try:
    from havok_snapshot import Image as _Base  # noqa: E402
except Exception:  # pragma: no cover - only without havok_snapshot
    class _Base:
        kind = "abstract"

        def describe(self):
            return {"kind": self.kind}


class SnapFormatError(ValueError):
    pass


def pages_of_ranges(ranges):
    """[[start, end), ...] guest ranges (ints or '0x..' strings) -> sorted unique page numbers."""
    out = set()
    for b, e in ranges:
        b = int(b, 0) if isinstance(b, str) else b
        e = int(e, 0) if isinstance(e, str) else e
        if e <= b:
            continue
        out.update(range(b >> 12, ((e - 1) >> 12) + 1))
    return sorted(out)


def write_snap(path, img, pages, meta=None):
    """Capture `pages` (page numbers) of `img` into `path`. A page the source returns short is
    stored with its valid length; a page it cannot read at all is listed in meta.missing.
    Returns {pages, partial, missing, file_bytes}."""
    table, blobs, missing, partial = [], [], [], 0
    for pg in sorted(set(pages)):
        if not 0 <= pg < MAX_PAGES:
            raise SnapFormatError("page 0x%X outside the 32-bit guest space" % pg)
        data = img.read(pg << 12, PAGE)
        if not data:
            missing.append("0x%08X" % (pg << 12))
            continue
        if len(data) < PAGE:
            partial += 1
        table.append((pg, len(data)))
        blobs.append(bytes(data))
    m = {"schema": SCHEMA, "source": img.describe(),
         "created": datetime.datetime.now().isoformat(timespec="seconds"), "missing": missing}
    m.update(meta or {})
    mj = json.dumps(m, sort_keys=True).encode("utf-8")
    body = bytearray(MAGIC)
    body += struct.pack("<I", len(mj)) + mj
    body += struct.pack("<I", len(table))
    for pg, n in table:
        body += struct.pack("<II", pg, n)
    for b in blobs:
        body += b
    body += struct.pack("<I", zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(body)
    return {"pages": len(table), "partial": partial, "missing": missing, "file_bytes": len(body)}


def parse_snap(blob):
    """-> (meta dict, {page number: bytes}). Raises SnapFormatError on any inconsistency."""
    if len(blob) < len(MAGIC) + 12 or blob[:len(MAGIC)] != MAGIC:
        raise SnapFormatError("not a guest snap (bad magic)")
    (crc,) = struct.unpack_from("<I", blob, len(blob) - 4)
    if zlib.crc32(blob[:-4]) & 0xFFFFFFFF != crc:
        raise SnapFormatError("CRC mismatch (truncated or modified file)")
    pos = len(MAGIC)
    (ml,) = struct.unpack_from("<I", blob, pos)
    pos += 4
    if pos + ml + 4 > len(blob) - 4:
        raise SnapFormatError("meta length past the end")
    try:
        meta = json.loads(blob[pos:pos + ml].decode("utf-8"))
    except ValueError as ex:
        raise SnapFormatError("meta is not JSON: %s" % ex)
    if meta.get("schema") != SCHEMA:
        raise SnapFormatError("schema %r, want %r" % (meta.get("schema"), SCHEMA))
    pos += ml
    (n,) = struct.unpack_from("<I", blob, pos)
    pos += 4
    if n > MAX_PAGES or pos + 8 * n > len(blob) - 4:
        raise SnapFormatError("page table past the end")
    table = [struct.unpack_from("<II", blob, pos + 8 * i) for i in range(n)]
    pos += 8 * n
    pages, prev = {}, -1
    for pg, ln in table:
        if pg <= prev:
            raise SnapFormatError("page table not strictly increasing at 0x%X" % pg)
        if pg >= MAX_PAGES or not 1 <= ln <= PAGE:
            raise SnapFormatError("bad page entry 0x%X len %d" % (pg, ln))
        prev = pg
        pages[pg] = bytes(blob[pos:pos + ln])
        pos += ln
    if pos != len(blob) - 4:
        raise SnapFormatError("%d stray bytes after the page data" % (len(blob) - 4 - pos))
    return meta, pages


class SnapImage(_Base):
    """A .snap as a guest image. read() returns the bytes present from va onward and stops at
    the first byte that is not captured (never zero-fills); `misses` counts such stops."""
    kind = "snap"

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.meta, self.pages = parse_snap(f.read())
        self.misses = 0

    def read(self, va, n):
        out = bytearray()
        va &= 0xFFFFFFFF
        while n > 0:
            pg = self.pages.get(va >> 12)
            off = va & 0xFFF
            if pg is None or off >= len(pg):
                self.misses += 1
                break
            take = min(n, len(pg) - off)
            out += pg[off:off + take]
            va += take
            n -= take
            if off + take < PAGE and n > 0:    # a partial page ends here
                self.misses += 1
                break
        return bytes(out)

    def iter_chunks(self, lo, hi):
        for pg in sorted(self.pages):
            va = pg << 12
            data = self.pages[pg]
            if va + len(data) <= lo or va >= hi:
                continue
            yield va, data

    def describe(self):
        src = self.meta.get("source") or {}
        return {"kind": self.kind, "path": os.path.abspath(self.path), "pages": len(self.pages),
                "label": self.meta.get("label"), "source_kind": src.get("kind"),
                "source_path": src.get("path"), "created": self.meta.get("created")}


class RecordingImage(_Base):
    """Wraps any image and records the page range of every read() REQUEST (not only what came
    back), so a page the source cannot serve still shows up as `missing` in the snap. Scans
    through iter_chunks() are passed through unrecorded: a scanning reader (havok_snapshot's
    class map) would pull in the whole address space and is out of scope for a .snap."""

    def __init__(self, inner):
        self.inner = inner
        self.kind = getattr(inner, "kind", "?")
        self.pages = set()

    def read(self, va, n):
        va &= 0xFFFFFFFF
        if n > 0:
            self.pages.update(range(va >> 12, ((va + n - 1) >> 12) + 1))
        return self.inner.read(va, n)

    def iter_chunks(self, lo, hi):
        return self.inner.iter_chunks(lo, hi)

    def describe(self):
        return self.inner.describe()


# ------------------------------------------------------------------ readers ----
def _objstate(img):
    import objstate
    snap = objstate.snapshot(img, objstate.DEFAULT_TRIGGER_DOCS, objstate.load_fsm_graphs(objstate.DEFAULT_AST))
    snap.pop("image", None)
    return snap


def _save_records(img):
    import save_records
    sch, info = save_records.schema_from_image(img)
    out = {"schema": {"pads": sch.pads, "stats": sch.stats, "entities": sch.entities,
                      "das": [list(x) for x in sch.das]}, "schema_info": info}
    repo_schema = save_records._schema(argparse.Namespace(schema=None))
    if repo_schema is not None:
        data, capture, finfo = save_records.file_from_image(img, repo_schema)
        out["file"] = data.hex() if data else None
        out["capture"] = capture.hex()
        out["file_info"] = finfo
    return out


READERS = {"objstate": _objstate, "save_records": _save_records}


def run_readers(img, names):
    return {n: READERS[n](img) for n in names}


def capture(spec, out, readers=("objstate", "save_records"), label=None, base=None, verify=True):
    """Run `readers` over the source image through a RecordingImage, write the union of the
    requested pages as a .snap, then (verify) re-run the same readers on the snap: the outputs
    must be identical and no read may fall outside the captured pages."""
    import havok_snapshot
    src = havok_snapshot.open_image(spec, base=base)
    rec = RecordingImage(src)
    want = run_readers(rec, readers)
    info = write_snap(out, src, sorted(rec.pages),
                      meta={"label": label or os.path.splitext(os.path.basename(out))[0],
                            "reader": "guest_snap.py capture " + ",".join(readers), "source_spec": spec})
    if verify:
        snap = SnapImage(out)
        got = run_readers(snap, readers)
        info["verified_equal"] = {n: _canon(want[n]) == _canon(got[n]) for n in readers}
        info["reads_outside_snap"] = snap.misses
    return info


def _canon(x):
    return json.dumps(x, sort_keys=True, default=str)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    ip = sub.add_parser("info")
    ip.add_argument("file")
    cp = sub.add_parser("capture")
    cp.add_argument("--image", required=True)
    cp.add_argument("--out", required=True)
    cp.add_argument("--ledger", help="capture these ranges instead of running the readers")
    cp.add_argument("--readers", default="objstate,save_records")
    cp.add_argument("--label")
    cp.add_argument("--base", type=lambda x: int(x, 0))
    args = ap.parse_args(argv)
    if args.cmd == "info":
        img = SnapImage(args.file)
        m = dict(img.meta)
        print(json.dumps({"describe": img.describe(), "meta": m,
                          "bytes": sum(len(p) for p in img.pages.values()),
                          "partial_pages": sum(1 for p in img.pages.values() if len(p) < PAGE)}, indent=1))
        return 0
    if args.cmd == "capture":
        if args.ledger:
            import havok_snapshot
            img = havok_snapshot.open_image(args.image, base=args.base)
            with open(args.ledger, encoding="utf-8") as f:
                led = json.load(f)
            info = write_snap(args.out, img, pages_of_ranges(led),
                              meta={"label": args.label or os.path.splitext(os.path.basename(args.out))[0],
                                    "reader": "guest_snap.py capture --ledger", "source_spec": args.image})
            print(json.dumps(info))
            return 0
        info = capture(args.image, args.out, [x for x in args.readers.split(",") if x], args.label, args.base)
        print(json.dumps(info))
        ok = all(info["verified_equal"].values()) and info["reads_outside_snap"] == 0
        print("self-check: readers on the snap %s the source; %d reads outside the snap"
              % ("EQUAL" if all(info["verified_equal"].values()) else "DIFFER FROM", info["reads_outside_snap"]))
        return 0 if ok else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
