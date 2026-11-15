#!/usr/bin/env python3
"""srvfs.py - the REFERENCE implementation of the mod virtual merge (docs/MODS.md sections 3-6).

The exe's kernel/mod_vfs.cpp implements the same rules; tools/mods/rt_mods.py checks the two
against each other byte for byte (build/difftest.exe --mod-vfs-compose) and against the
retail disc (a no-op composition returns every file byte-identical).

Model
  Src      a random-access byte source: a list of segments (a range of a host file, bytes in
           memory, zeros). Composition never copies a big file: it builds segment lists.
  Bgfa     one BGFA1.05 container parsed from a Src (header widths/shift honoured as
           BigFileToc_GetEntryOffset_sub_82706170 reads them).
  build()  the canonical re-layout every retail container follows (census 2026-10-02:
           1,390/1,390): TOC-last = data in TOC order from align(0x40, 1<<shift), each entry
           aligned, TOC 4-aligned after the last entry, end = TOC end; TOC-first = TOC at 0x40
           with ids sorted (sign-extended LE key, unsigned compare: sub_82705ED8), data from
           align(TOC end, 1<<shift) in the original data order, new entries last, end = last
           data end.
  Layer    a port feature or a mod: whole files + entry ops (replace / add / SRPATCH1 patch).
  plan()   disc + layers (bottom-up) -> {file rel: FilePlan(src, conflicts)} for every file
           some layer changed.

No third-party dependencies (stdlib only).
"""
from __future__ import annotations

import hashlib
import mmap
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import srpatch  # noqa: E402

MAGIC = b"BGFA1.05"
HDR = 0x40
FLATTEN_LIMIT = 8 << 20      # composed files below this are flattened into memory (exe too)


class VfsError(Exception):
    """A layer's content cannot be applied (maps to status bad-content / bad-patch)."""

    def __init__(self, msg, status="bad-content"):
        super().__init__(msg)
        self.status = status


# ------------------------------------------------------------------ hashing
def fnv1a64_upper(name: str) -> int:
    h = 0xCBF29CE484222325
    for c in name.upper().encode("latin1"):
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def align(n, a):
    return (n + a - 1) // a * a


# ------------------------------------------------------------------ sources
_MMAPS = {}


def _mm(path):
    m = _MMAPS.get(path)
    if m is None:
        f = open(path, "rb")
        if os.fstat(f.fileno()).st_size == 0:
            m = b""
        else:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        _MMAPS[path] = m
    return m


class Src:
    """segments: list of (length, kind, obj, off): kind 'f' = host file path obj at off,
    'm' = bytes obj at off, 'z' = zeros."""
    __slots__ = ("segs", "size", "widened")

    def __init__(self, segs=()):
        self.segs = []
        self.size = 0
        self.widened = None
        for s in segs:
            self._add(s)

    def _add(self, s):
        if s[0] <= 0:
            return
        if self.segs:
            L, k, o, off = self.segs[-1]
            if k == s[1] and o is s[2] and k != "z" and off + L == s[3]:
                self.segs[-1] = (L + s[0], k, o, off)
                self.size += s[0]
                return
            if k == "z" and s[1] == "z":
                self.segs[-1] = (L + s[0], k, o, off)
                self.size += s[0]
                return
        self.segs.append(s)
        self.size += s[0]

    @staticmethod
    def file(path, off=0, length=None):
        if length is None:
            length = os.path.getsize(path) - off
        return Src([(length, "f", path, off)])

    @staticmethod
    def mem(b):
        b = bytes(b)
        return Src([(len(b), "m", b, 0)])

    @staticmethod
    def zero(n):
        return Src([(n, "z", None, 0)])

    @staticmethod
    def concat(parts):
        out = Src()
        for p in parts:
            for s in p.segs:
                out._add(s)
        return out

    def slice(self, off, n):
        if off < 0 or n < 0 or off + n > self.size:
            raise VfsError("slice %#x+%#x outside a %#x-byte source" % (off, n, self.size))
        out = Src()
        pos = 0
        for L, k, o, so in self.segs:
            if n <= 0:
                break
            if pos + L <= off:
                pos += L
                continue
            a = max(off, pos)
            take = min(pos + L, off + n) - a
            out._add((take, k, o, so + (a - pos)))
            n -= take
            off = a + take
            pos += L
        return out

    def read(self, off, n):
        n = max(0, min(n, self.size - off))
        out = bytearray()
        pos = 0
        for L, k, o, so in self.segs:
            if n <= 0:
                break
            if pos + L <= off:
                pos += L
                continue
            a = off - pos
            take = min(L - a, n)
            if k == "f":
                out += _mm(o)[so + a: so + a + take]
            elif k == "m":
                out += o[so + a: so + a + take]
            else:
                out += bytes(take)
            off += take
            n -= take
            pos += L
        return bytes(out)

    def tobytes(self):
        return self.read(0, self.size)

    def chunks(self, step=1 << 22):
        for L, k, o, so in self.segs:
            p = 0
            while p < L:
                t = min(step, L - p)
                if k == "f":
                    yield _mm(o)[so + p: so + p + t]
                elif k == "m":
                    yield o[so + p: so + p + t]
                else:
                    yield bytes(t)
                p += t

    def sha256(self):
        h = hashlib.sha256()
        for c in self.chunks():
            h.update(c)
        return h.hexdigest()

    def write(self, path):
        with open(path, "wb") as f:
            for c in self.chunks():
                f.write(c)

    def equals(self, other):
        if self.size != other.size:
            return False
        step = 1 << 20
        for p in range(0, self.size, step):
            if self.read(p, step) != other.read(p, step):
                return False
        return True

    def is_bgfa(self):
        return self.size >= HDR and self.read(0, 8) == MAGIC

    def flattened(self):
        return Src.mem(self.tobytes()) if self.size < FLATTEN_LIMIT else self


# ------------------------------------------------------------------ RefPack header
def refpack_size(head):
    """-> decompressed size from a RefPack stream header, or None if `head` is not one."""
    if len(head) < 5 or head[1] != 0xFB or (head[0] & 0x3E) != 0x10:
        return None
    w = 4 if head[0] & 0x80 else 3
    p = 2 + (w if head[0] & 1 else 0)
    if len(head) < p + w:
        return None
    return int.from_bytes(head[p:p + w], "big")


# ------------------------------------------------------------------ containers
def key_of(ident, wId):
    """sort key of an id as ResCatalogTable_BsearchCmpKey_sub_82705ED8 compares it."""
    if wId >= 8:
        return ident & 0xFFFFFFFFFFFFFFFF
    sign = 1 << (8 * wId - 1)
    v = ((ident & ((1 << (8 * wId)) - 1)) ^ sign) - sign
    return v & 0xFFFFFFFFFFFFFFFF


class Ent:
    __slots__ = ("lead", "a", "id", "name_raw", "off", "unc", "data", "order", "orig", "slack")

    def __init__(self, lead, a, ident, name_raw, off, unc, data, order):
        self.lead, self.a, self.id, self.name_raw = lead, a, ident, name_raw
        self.off, self.unc, self.data, self.order = off, unc, data, order
        self.orig = data          # the parsed bytes (data is orig = unchanged)
        self.slack = 0            # original bytes after the entry up to the next item

    @property
    def name(self):
        return self.name_raw.split(b"\0")[0].decode("latin1") if self.name_raw else ""

    @property
    def size(self):
        return self.data.size


class Bgfa:
    def __init__(self, src: Src):
        if not src.is_bgfa():
            raise VfsError("not a BGFA1.05 container")
        h = src.read(0, HDR)
        self.src = src
        self.hdr = bytearray(h)
        count, toc_off = struct.unpack_from("<II", h, 0x0C)
        self.mode = h[0x20]
        self.wA, self.wId, self.wOff, self.wSize, self.wUnc, self.shift = h[0x21:0x27]
        self.ntags, self.namelen = struct.unpack_from("<II", h, 0x28)
        if (self.wId < 1 and count) or max(self.wA, self.wId, self.wOff, self.wSize, self.wUnc) > 8:
            raise VfsError("container header widths out of range")      # (empty retail streams: all widths 0)
        recsz = 1 + self.wA + self.wId + self.wOff + self.wSize + self.wUnc + self.namelen
        toc_len = 4 * self.ntags + recsz * count
        if toc_off + toc_len > src.size:
            raise VfsError("container TOC runs past its end")
        toc = src.read(toc_off, toc_len)
        self.tags = list(struct.unpack_from("<%dI" % self.ntags, toc, 0)) if self.ntags else []
        p = 4 * self.ntags
        self.entries = []
        for i in range(count):
            r = toc[p:p + recsz]
            p += recsz
            q = 1
            a = int.from_bytes(r[q:q + self.wA], "little"); q += self.wA
            ident = int.from_bytes(r[q:q + self.wId], "little"); q += self.wId
            off = int.from_bytes(r[q:q + self.wOff], "little") << self.shift; q += self.wOff
            size = int.from_bytes(r[q:q + self.wSize], "little"); q += self.wSize
            unc = int.from_bytes(r[q:q + self.wUnc], "little") if self.wUnc else 0; q += self.wUnc
            name_raw = bytes(r[q:q + self.namelen])
            if off + size > src.size:
                raise VfsError("entry %d (%#x+%#x) runs past its container" % (i, off, size))
            self.entries.append(Ent(r[0], a, ident, name_raw, off, unc, src.slice(off, size), i))
        first = min((e.off for e in self.entries), default=None)
        self.toc_first = (toc_off < first) if first is not None else (self.mode == 1)
        # Padding is not always zero in retail (lunits.AST: garbage after RefPack entries;
        # 79 ui.AST movies: garbage after the TOC). A rebuild re-emits the ORIGINAL bytes
        # after an unchanged entry (its slack) and after the header, zeros elsewhere, so an
        # identity rebuild is byte-exact (1,390/1,390 containers + the 29 empty streams).
        bounds = sorted(set([e.off for e in self.entries] + [toc_off, src.size]))
        import bisect
        for e in self.entries:
            end = e.off + e.size
            k = bisect.bisect_left(bounds, end)
            while k < len(bounds) and bounds[k] == e.off and e.size == 0:
                k += 1
            e.slack = (bounds[k] - end) if k < len(bounds) else 0
        self.head_gap = ((first if first is not None else toc_off) - HDR) if not self.toc_first else 0
        # TOC-first: the region between the header and the first entry. Retail leaves
        # authoring-tool garbage after the TOC there (79 ui.AST movies); a rebuild keeps the
        # region (new TOC written over it) whenever the new TOC fits - tools/apt/bgfa.py's
        # rebuild() does the same - so an identity rebuild is byte-exact.
        self.region_end = (first if first is not None else toc_off + toc_len) if self.toc_first else None
        self.changed = False

    # -------------------------------------------------------------- lookup
    def find(self, comp: str):
        """docs/MODS.md section 3 resolution -> (index or None, id to use for an add)."""
        if comp.startswith("#"):
            ident = int(comp[1:], 16)
            for i, e in enumerate(self.entries):
                if e.id == ident:
                    return i, ident
            return None, ident
        if self.namelen:
            u = comp.upper()
            for i, e in enumerate(self.entries):
                if e.name.upper() == u:
                    return i, fnv1a64_upper(comp)
        h = fnv1a64_upper(comp) & ((1 << (8 * self.wId)) - 1)
        for i, e in enumerate(self.entries):
            if e.id == h:
                return i, h
        if len(comp) == 16 and all(c in "0123456789abcdefABCDEF" for c in comp):
            ident = int(comp, 16)
            for i, e in enumerate(self.entries):
                if e.id == ident:
                    return i, ident
            if not self.namelen:
                return None, ident           # rule 4: an id (unnamed containers only)
        return None, h

    def label(self, i):
        e = self.entries[i]
        return e.name if self.namelen and e.name else "#%016x" % e.id

    # -------------------------------------------------------------- edit
    def _container_lead(self):
        for e in self.entries:
            return e.lead & ~0x02
        return 0x00 if self.mode == 1 else 0x10

    def _check_data(self, data: Src, lead):
        """-> (lead, unc) for `data` stored in this container."""
        if self.wUnc:
            dec = refpack_size(data.read(0, 8))
            if dec is None:
                raise VfsError("this container stores RefPack entries; the new entry is not a RefPack stream")
            unc = dec - data.size
            if unc < 0:
                raise VfsError("RefPack entry larger than its decompressed size")
            return 0x12, unc
        return lead, 0

    def replace(self, i, data: Src, meta=None):
        e = self.entries[i]
        lead = meta["lead"] if meta and "lead" in meta else e.lead
        lead, unc = self._check_data(data, lead)
        e.lead, e.unc, e.data = lead, unc, data
        if meta and "a" in meta:
            e.a = meta["a"]
        self.changed = True

    def add(self, comp, ident, data: Src, meta=None):
        meta = meta or {}
        if self.namelen:
            name = meta.get("name", comp if not comp.startswith("#") else "")
            if not name:
                raise VfsError("a new entry in a named container needs a name (not %r)" % comp)
            nb = name.encode("latin1")
            if len(nb) >= self.namelen:
                raise VfsError("name %r does not fit this container's %d-byte names" % (name, self.namelen))
            name_raw = meta.get("name_raw", nb + bytes(self.namelen - len(nb)))
        else:
            name_raw = b""
        if "a" in meta:
            a = meta["a"]
        else:
            a = self._infer_a(data)
        if self.wA == 0 and a != 0:
            raise VfsError("this container has no type field (type must be 0)")
        if self.mode == 1 and self.ntags and a >= self.ntags:
            raise VfsError("type %d is not one of this container's %d tags" % (a, self.ntags))
        lead = meta.get("lead", self._container_lead())
        lead, unc = self._check_data(data, lead)
        if any(e.id == ident for e in self.entries):
            raise VfsError("id %016x already present" % ident)
        if self.wId == 0:
            if not self.ntags:
                raise VfsError("cannot add to an empty container that has no type tags")
            self.wId = 8
            self.hdr[0x22] = 8
        self.entries.append(Ent(lead, a, ident, name_raw, None, unc, data, len(self.entries)))
        self.changed = True
        return len(self.entries) - 1

    def _infer_a(self, data: Src):
        if self.wA == 0 or len(set(e.a for e in self.entries)) <= 1:
            return self.entries[0].a if self.entries else 0
        head = data.read(0, 8)
        for e in self.entries:
            if e.data.read(0, 8 if head == MAGIC else 4) == head[:8 if head == MAGIC else 4]:
                return e.a
        raise VfsError("cannot infer the type of a new entry here; give \"type\" in mod.json")

    # -------------------------------------------------------------- build
    def build(self) -> Src:
        if not self.changed:
            return self.src
        al = 1 << self.shift
        ents = self.entries
        data_order = sorted(range(len(ents)),
                            key=lambda i: (ents[i].off is None, ents[i].off if ents[i].off is not None else ents[i].order))
        if self.toc_first:
            data_order_final = data_order
        else:
            data_order_final = list(range(len(ents)))       # TOC-last: data in TOC order
        rec_ents = list(range(len(ents)))
        if self.toc_first:
            rec_ents.sort(key=lambda i: key_of(ents[i].id, self.wId))
            for x, y in zip(rec_ents, rec_ents[1:]):
                if key_of(ents[x].id, self.wId) == key_of(ents[y].id, self.wId):
                    raise VfsError("duplicate id %016x in a sorted container" % ents[x].id)
        hdr = bytearray(self.hdr)
        widened = []
        wOff, wSize, wUnc = self.wOff, self.wSize, self.wUnc
        for _ in range(8):
            recsz = 1 + self.wA + self.wId + wOff + wSize + wUnc + self.namelen
            toc_len = 4 * self.ntags + recsz * len(ents)
            if self.toc_first:
                cur = self.region_end if HDR + toc_len <= self.region_end else HDR + toc_len
            else:
                cur = HDR
            offs = {}
            for i in data_order_final:
                cur = align(cur, al)
                offs[i] = cur
                cur += ents[i].size
            if not ents:
                break
            need_off = max([(offs[i] >> self.shift) for i in offs] + [0])
            need_size = max([e.size for e in ents] + [0])
            need_unc = max([e.unc for e in ents] + [0])
            nOff, nSize = max(wOff, nbytes(need_off)), max(wSize, nbytes(need_size))
            nUnc = max(wUnc, nbytes(need_unc)) if wUnc else 0
            if (nOff, nSize, nUnc) == (wOff, wSize, wUnc):
                break
            widened.append((wOff, wSize, wUnc, nOff, nSize, nUnc))
            wOff, wSize, wUnc = nOff, nSize, nUnc
        else:
            raise VfsError("field widths do not converge")
        hdr[0x23], hdr[0x24], hdr[0x25] = wOff, wSize, wUnc
        toc = bytearray()
        for t in self.tags:
            toc += struct.pack("<I", t)
        for i in rec_ents:
            e = ents[i]
            rec = bytearray([e.lead])
            rec += e.a.to_bytes(self.wA, "little")
            rec += (e.id & ((1 << (8 * self.wId)) - 1)).to_bytes(self.wId, "little")
            rec += (offs[i] >> self.shift).to_bytes(wOff, "little")
            rec += e.size.to_bytes(wSize, "little")
            if wUnc:
                rec += e.unc.to_bytes(wUnc, "little")
            rec += e.name_raw
            toc += rec
        parts = []

        def pad(after, n, pos):
            """n bytes of padding at pos, after entry index `after` (None = the header)."""
            if n <= 0:
                return
            if after is None:
                avail, so = (self.head_gap if pos == HDR else 0), HDR
            else:
                e = ents[after]
                avail, so = ((e.slack, e.off + e.size) if e.data is e.orig and e.off is not None else (0, 0))
            take = min(n, avail)
            if take:
                parts.append(self.src.slice(so, take))
            if n > take:
                parts.append(Src.zero(n - take))

        if self.toc_first:
            toc_off = HDR
            parts.append(None)          # header placeholder
            parts.append(Src.mem(toc))
            pos = HDR + len(toc)
            if pos < self.region_end:   # keep the rest of the original region (see __init__)
                parts.append(self.src.slice(pos, self.region_end - pos))
                pos = self.region_end
        else:
            parts.append(None)
            pos = HDR
        prev = None
        for i in data_order_final:
            pad(prev, offs[i] - pos, pos)
            parts.append(ents[i].data)
            pos = offs[i] + ents[i].size
            prev = i
        if not self.toc_first:
            toc_off = align(pos, 4)
            pad(prev, toc_off - pos, pos)
            parts.append(Src.mem(toc))
        struct.pack_into("<I", hdr, 0x0C, len(ents))
        struct.pack_into("<I", hdr, 0x10, toc_off)
        struct.pack_into("<I", hdr, 0x18, len(toc))
        parts[0] = Src.mem(bytes(hdr))
        out = Src.concat(parts)
        out.widened = widened if widened else None
        return out


def nbytes(v):
    n = 1
    while v >= 1 << (8 * n):
        n += 1
    return n


# ------------------------------------------------------------------ edit trees
class Op:
    """One layer's operation on one entry: kind 'bytes' (Src + meta) or 'patch' (SRPATCH1 bytes)."""
    __slots__ = ("kind", "src", "patch", "meta", "label")

    def __init__(self, kind, src=None, patch=None, meta=None, label=""):
        self.kind, self.src, self.patch, self.meta, self.label = kind, src, patch, meta, label


class Node:
    __slots__ = ("op", "children")

    def __init__(self):
        self.op = None
        self.children = {}       # comp (as written) -> Node, insertion order kept


def tree_insert(root: Node, comps, op: Op):
    n = root
    for c in comps:
        n = n.children.setdefault(c, Node())
    if n.op is not None:
        raise VfsError("two edits of %s in one layer" % "/".join(comps), "bad-manifest")
    n.op = op


class Ctx:
    """Bookkeeping for one file across layers."""

    def __init__(self, rel, disc_src, layer_name, log):
        self.rel = rel
        self.disc = disc_src            # the disc file's Src or None
        self.layer = layer_name
        self.writers = {}               # path tuple of ids -> (layer, display)
        self.conflicts = []             # (display entry, winner, loser)
        self.log = log

    def note_write(self, path, display):
        lost = [p for p in self.writers if p[:len(path)] == path and self.writers[p][0] != self.layer]
        for p in lost:
            w, disp = self.writers.pop(p)
            self.conflicts.append((self.rel + ":" + disp, self.layer, w))
        self.writers[path] = (self.layer, display)


def disc_entry(ctx: Ctx, idpath):
    """the disc's bytes of the entry at idpath (tuple of ids), or None."""
    if ctx.disc is None:
        return None
    src = ctx.disc
    for ident in idpath:
        if not src.is_bgfa():
            return None
        c = Bgfa(src)
        hit = [e for e in c.entries if e.id == ident]
        if not hit:
            return None
        src = hit[0].data
    return src


def apply_patch(ctx, cur: Src, patch: bytes, idpath, display):
    """SRPATCH1 on the entry's current bytes; falls back to the disc's bytes (the lower
    layers' edits of this entry are then lost: conflicts). -> (Src, stacked?)"""
    try:
        hdr, ops = srpatch.parse(patch)
    except srpatch.PatchError as ex:
        raise VfsError("%s: %s" % (display, ex), "bad-patch")
    for cand, stacked in ((cur, True), (disc_entry(ctx, idpath), False)):
        if cand is None or cand.size != hdr["base_size"]:
            continue
        base = cand.tobytes()
        if hashlib.sha256(base).digest() != hdr["base_sha256"]:
            continue
        filefn = (lambda off, n: ctx.disc.read(off, n)) if ctx.disc is not None else None
        try:
            out = srpatch.apply_ops(base, ops, filefn)
        except srpatch.PatchError as ex:
            raise VfsError("%s: %s" % (display, ex), "bad-patch")
        if len(out) != hdr["out_size"] or hashlib.sha256(out).digest() != hdr["out_sha256"]:
            raise VfsError("%s: the patch output does not match its hash" % display, "bad-patch")
        return Src.mem(out), stacked
    raise VfsError("%s: the patch was made for other bytes (base size/SHA-256 mismatch)" % display, "bad-patch")


def apply_node(ctx: Ctx, src, node: Node, idpath, display) -> Src:
    """Apply `node` (one layer's edits at this entry) to `src` (None = absent)."""
    if node.op is not None:
        op = node.op
        if op.kind == "bytes":
            src = op.src
        else:
            if src is None:
                raise VfsError("%s: a patch needs an existing entry" % display, "bad-patch")
            src, stacked = apply_patch(ctx, src, op.patch, idpath, display)
        ctx.note_write(idpath, display)
    if not node.children:
        return src
    if src is None:
        raise VfsError("%s: no such container to edit inside" % display)
    if not src.is_bgfa():
        raise VfsError("%s is not a container" % display)
    c = Bgfa(src)
    for comp, child in node.children.items():
        i, ident = c.find(comp)
        cdisp = (display + "/" if display else "") + (c.label(i) if i is not None else comp)
        cpath = idpath + (ident if i is None else c.entries[i].id,)
        if i is None:
            if child.op is None or child.op.kind != "bytes":
                raise VfsError("%s: no such entry" % cdisp)
            data = apply_node(ctx, None, child, cpath, cdisp)
            c.add(comp, ident, data, child.op.meta)
        else:
            before = c.entries[i].data
            data = apply_node(ctx, before, child, cpath, cdisp)
            if data is not before:
                meta = child.op.meta if child.op is not None else None
                if data.equals(before) and not meta:
                    continue
                c.replace(i, data, meta)
    out = c.build()
    if getattr(out, "widened", None):
        ctx.log("[MOD] widened %s:%s fields %s" % (ctx.rel, display or "<file>", out.widened))
    return out


# ------------------------------------------------------------------ auto-split
def same_shape(a: Bgfa, b: Bgfa):
    return (a.mode == b.mode and a.wA == b.wA and a.wId == b.wId and a.shift == b.shift and
            (a.wUnc == 0) == (b.wUnc == 0) and a.ntags == b.ntags and a.tags == b.tags and
            a.namelen == b.namelen and a.toc_first == b.toc_first and a.hdr[8:12] == b.hdr[8:12])


def split(mod: Src, disc: Src, comps=(), out=None):
    """Whole-file layer -> entry ops relative to `disc` (docs/MODS.md 5 auto-split).
    -> list of (comps, Op) or None when the file cannot be expressed as entry edits."""
    if out is None:
        out = []
    if not (mod.is_bgfa() and disc.is_bgfa()):
        return None
    m, d = Bgfa(mod), Bgfa(disc)
    if not same_shape(m, d):
        return None
    by_id = {e.id: e for e in d.entries}
    if len(by_id) != len(d.entries):
        return None
    mids = set(e.id for e in m.entries)
    if any(i not in mids for i in by_id):
        return None                          # an entry removed: whole file
    for e in m.entries:
        comp = "#%016x" % e.id
        meta = {"lead": e.lead, "a": e.a}
        if m.namelen:
            meta["name_raw"] = e.name_raw
            meta["name"] = e.name
        de = by_id.get(e.id)
        if de is None:
            out.append((comps + (comp,), Op("bytes", src=e.data, meta=meta, label="split-add")))
            continue
        same_meta = (de.lead == e.lead and de.a == e.a and (not m.namelen or de.name_raw == e.name_raw))
        if same_meta and e.data.equals(de.data):
            continue
        if same_meta and e.data.is_bgfa() and de.data.is_bgfa():
            sub = split(e.data, de.data, comps + (comp,), [])
            if sub is not None:
                out.extend(sub)
                continue
        out.append((comps + (comp,), Op("bytes", src=e.data, meta=meta, label="split-replace")))
    return out


# ------------------------------------------------------------------ layers and the plan
class Layer:
    def __init__(self, ident, name, kind):
        self.id, self.name, self.kind = ident, name, kind
        self.whole = {}          # lower(rel) -> (rel, Src)
        self.entries = {}        # lower(rel) -> (rel, [(comps, Op)])

    def add_entry(self, rel, comps, op):
        k = rel.lower()
        self.entries.setdefault(k, (rel, []))[1].append((tuple(comps), op))


class FilePlan:
    def __init__(self, rel, src, conflicts, layers):
        self.rel, self.src, self.conflicts, self.layers = rel, src, conflicts, layers


def plan_file(rel, disc_src, layers, log=print):
    """-> FilePlan or None (no layer changes the file). Raises VfsError(layer=...)"""
    k = rel.lower()
    touching = [L for L in layers if k in L.whole or k in L.entries]
    if not touching:
        return None
    src = disc_src
    ctx = Ctx(rel, disc_src, None, log)
    for L in touching:
        # auto-split only when the layers below changed this file (else the whole file is
        # simply the base for the layers above: same result, no compare cost)
        need_split = src is not disc_src
        ctx.layer = L.id
        try:
            ops = []
            if k in L.whole:
                wsrc = L.whole[k][1]
                sp = None
                if need_split and disc_src is not None:
                    sp = split(wsrc, disc_src)
                if sp is None:
                    ctx.note_write((), "<whole file>")
                    src = wsrc
                else:
                    log("[MOD] split %s (%s): whole file -> %d entry edit(s)" % (rel, L.id, len(sp)))
                    ops.extend(sp)
            if k in L.entries:
                ops.extend(L.entries[k][1])
            if ops:
                root = Node()
                for comps, op in ops:
                    tree_insert(root, comps, op)
                src = apply_node(ctx, src, root, (), "")
        except VfsError as ex:
            ex.layer = L.id
            raise
    if src is disc_src:
        return None
    if disc_src is not None and src.size == disc_src.size and src.equals(disc_src):
        return None
    return FilePlan(rel, src.flattened(), ctx.conflicts, [L.id for L in touching])


def plan(disc_root, layers, log=print):
    """-> ({lower(rel): FilePlan}, {layer id: (status, reason)} failed). A failing layer is
    removed and the plan rebuilt without it (docs/MODS.md section 8)."""
    failed = {}
    active = list(layers)
    while True:
        files = {}
        for L in active:
            for k, (rel, _) in list(L.whole.items()) + list(L.entries.items()):
                files.setdefault(k, rel)
        out = {}
        bad = None
        for k, rel in sorted(files.items()):
            p = os.path.join(disc_root, rel)
            disc_src = Src.file(p) if os.path.isfile(p) else None
            try:
                fp = plan_file(rel, disc_src, active, log)
            except VfsError as ex:
                bad = (ex.layer, ex.status, "%s: %s" % (rel, ex))
                break
            if fp is not None:
                out[k] = fp
        if bad is None:
            return out, failed
        failed[bad[0]] = (bad[1], bad[2])
        log("[MOD] error %s: %s" % (bad[0], bad[2]))
        active = [L for L in active if L.id != bad[0]]


# ------------------------------------------------------------------ layer loading
def load_entries_tree(layer, root):
    """entries/<disc rel container path>/<comp>/... -> ops. A FILE is new bytes, a folder
    edits inside, '<comp>.srpatch' is a delta. The container path is the longest prefix of
    the folder chain that names a FILE path on disc (matched as written; the disc's case is
    not needed: rel paths are case-insensitive)."""
    eroot = os.path.join(root, "entries")
    if not os.path.isdir(eroot):
        return
    for dirpath, dirnames, filenames in os.walk(eroot):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            parts = os.path.relpath(full, eroot).replace("\\", "/").split("/")
            split_at = None
            for i, p in enumerate(parts[:-1]):
                if p.upper().endswith(".AST"):
                    split_at = i
                    break
            if split_at is None:
                raise VfsError("entries/%s: no container (.AST) folder in the path" % "/".join(parts), "bad-manifest")
            rel = "/".join(parts[:split_at + 1])
            comps = parts[split_at + 1:]
            if comps[-1].lower().endswith(".srpatch"):
                comps[-1] = comps[-1][:-len(".srpatch")]
                layer.add_entry(rel, comps, Op("patch", patch=open(full, "rb").read(), label=full))
            else:
                layer.add_entry(rel, comps, Op("bytes", src=Src.file(full), label=full))


def parse_target(target):
    """'DATA/text.AST:INTERFACETEXT' or 'DATA/objtiv.AST/A/B' -> (rel, [comps])."""
    if ":" in target:
        rel, rest = target.split(":", 1)
    else:
        parts = target.replace("\\", "/").split("/")
        cut = None
        for i, p in enumerate(parts[:-1]):
            if p.upper().endswith(".AST"):
                cut = i
                break
        if cut is None:
            raise VfsError("target %r names no container" % target, "bad-manifest")
        rel, rest = "/".join(parts[:cut + 1]), "/".join(parts[cut + 1:])
    comps = [c for c in rest.replace("\\", "/").split("/") if c]
    rel = rel.replace("\\", "/").strip("/")
    if not comps or not rel or any(c in (".", "..") for c in rel.split("/")):
        raise VfsError("bad target %r" % target, "bad-manifest")
    return rel, comps


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Compose mods over the disc (reference).")
    ap.add_argument("--disc", required=True, help="the 'Superman Returns (USA)' folder")
    ap.add_argument("--mod", action="append", default=[], help="mod folder (repeat; FIRST = highest priority)")
    ap.add_argument("--out", help="write every composed file here")
    a = ap.parse_args(argv)
    layers = []
    for path in reversed(a.mod):
        L = Layer(os.path.basename(path.rstrip("/\\")), os.path.basename(path), "mod")
        load_entries_tree(L, path)
        for dp, dn, fns in os.walk(path):
            relp = os.path.relpath(dp, path).replace("\\", "/")
            if relp.split("/")[0] in ("entries",) or relp.startswith("."):
                continue
            for fn in fns:
                if relp == "." and fn.lower() == "mod.json":
                    continue
                rel = fn if relp == "." else relp + "/" + fn
                L.whole[rel.lower()] = (rel, Src.file(os.path.join(dp, fn)))
        layers.append(L)
    files, failed = plan(a.disc, layers)
    for k, fp in sorted(files.items()):
        print("%s %d %s conflicts=%s" % (fp.rel, fp.src.size, fp.src.sha256(), fp.conflicts))
        if a.out:
            dst = os.path.join(a.out, fp.rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            fp.src.write(dst)
    for k, v in failed.items():
        print("FAILED", k, v)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
