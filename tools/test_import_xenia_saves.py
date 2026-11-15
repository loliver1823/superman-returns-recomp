#!/usr/bin/env python3
"""Tests for tools/import_xenia_saves.py (python tools/test_import_xenia_saves.py).

The Xenia header layouts are rebuilt here INDEPENDENTLY of the tool's hard-coded
offsets: ctypes big-endian structures transcribed field by field from
reference/xenia-canary-oracle/src/xenia/kernel/xam/xcontent/xcontent.h (pack 1),
so a wrong offset in the tool fails the layout test and the synthetic-tree tests
(headers are built through these structures, not through the tool).
"""
from __future__ import annotations

import ctypes as C
import io
import json
import struct
import sys
import shutil
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import import_xenia_saves as T  # noqa: E402

BE = C.BigEndianStructure
U8, U16, U32, U64 = C.c_uint8, C.c_uint16, C.c_uint32, C.c_uint64


class XContentLicense(BE):
    _pack_ = 1
    _fields_ = [("licensee_id", U64), ("license_bits", U32), ("license_flags", U32)]


class ExecInfo(BE):   # xex2_opt_execution_info
    _pack_ = 1
    _fields_ = [("media_id", U32), ("version_value", U32), ("base_version_value", U32),
                ("title_id", U32), ("platform", U8), ("executable_table", U8),
                ("disc_number", U8), ("disc_count", U8), ("savegame_id", U32)]


class XContentMetadata(BE):
    _pack_ = 1
    _fields_ = [
        ("content_type", U32), ("metadata_version", U32), ("content_size", U64),
        ("execution_info", ExecInfo), ("console_id", U8 * 5), ("profile_id", U64),
        ("volume_descriptor", U8 * 0x24),            # StfsVolumeDescriptor / SvodDeviceDescriptor
        ("data_file_count", U32), ("data_file_size", U64), ("volume_type", U32),
        ("online_creator", U64), ("category", U32), ("reserved2", U8 * 0x20),
        ("metadata_v2", U8 * 0x24),                  # XContentMediaData / AvatarAssetData
        ("device_id", U8 * 0x14),
        ("display_name_raw", (U16 * 128) * 9), ("description_raw", (U16 * 128) * 9),
        ("publisher_raw", U16 * 64), ("title_name_raw", U16 * 64),
        ("flags", U8), ("thumbnail_size", U32), ("title_thumbnail_size", U32),
        ("thumbnail", U8 * 0x3D00), ("display_name_ex_raw", (U16 * 128) * 3),
        ("title_thumbnail", U8 * 0x3D00), ("description_ex_raw", (U16 * 128) * 3),
    ]


class XContentHeader(BE):
    _pack_ = 1
    _fields_ = [("magic", U32), ("signature", U8 * 0x228), ("licenses", XContentLicense * 16),
                ("content_id", U8 * 0x14), ("header_size", U32)]


class XContentContainerHeader(BE):
    _pack_ = 1
    _fields_ = [("content_header", XContentHeader), ("content_metadata", XContentMetadata),
                ("file_name_raw", C.c_char * 42)]


class XContentDataAggregate(BE):   # XCONTENT_DATA_AGGREGATE (natural alignment, 0x148)
    _fields_ = [("device_id", U32), ("content_type", U32), ("display_name_raw", U16 * 128),
                ("file_name_raw", C.c_char * 42), ("padding", U8 * 2),
                ("xuid", U64), ("title_id", U32)]


MAGIC_CON = 0x434F4E20


def set_wide(arr, s: str):
    for i in range(len(arr)):
        arr[i] = 0
    units = struct.unpack(">%dH" % (len(s.encode("utf-16-be")) // 2), s.encode("utf-16-be"))
    for i, u in enumerate(units[:len(arr) - 1]):
        arr[i] = u


def container_header(display: str = T.GAME_DISPLAY_NAME, file_name: str = T.GAME_FILE_NAME,
                     content_type=1, title=T.TITLE_ID, profile=0xE03000007EB97E5D, lang_slot=0,
                     thumb=b"", license_bits=0, pad_to_4k=True, version=2, license_flags=1) -> bytes:
    """What current Xenia's ContentPackage::InitializePackageHeader + ExtractContentHeader write."""
    h = XContentContainerHeader()
    h.content_header.magic = MAGIC_CON
    h.content_header.header_size = C.sizeof(XContentHeader) + C.sizeof(XContentMetadata)
    m = h.content_metadata
    m.content_type = content_type
    m.metadata_version = version
    m.data_file_count = 1
    m.volume_type = 0
    m.execution_info.title_id = title
    m.profile_id = profile
    if lang_slot < 9:
        set_wide(m.display_name_raw[lang_slot], display)
    else:
        set_wide(m.display_name_ex_raw[lang_slot - 9], display)
    if thumb:
        m.thumbnail_size = len(thumb)
        C.memmove(C.addressof(m.thumbnail), thumb, len(thumb))
    if license_bits:
        h.content_header.licenses[3].license_bits = license_bits
        h.content_header.licenses[3].license_flags = license_flags
    h.file_name_raw = file_name.encode("latin-1")
    raw = bytes(h)
    if pad_to_4k:
        raw += b"\0" * (((len(raw) + 0xFFF) & ~0xFFF) - len(raw))
    else:
        raw = raw[:C.sizeof(XContentHeader) + C.sizeof(XContentMetadata)]
    return raw


def aggregate_header(display=T.GAME_DISPLAY_NAME, file_name=T.GAME_FILE_NAME, xuid=0, license=None) -> bytes:
    a = XContentDataAggregate()
    a.device_id, a.content_type = 1, 1
    set_wide(a.display_name_raw, display)
    a.file_name_raw = file_name.encode("latin-1")
    a.xuid, a.title_id = xuid, T.TITLE_ID
    raw = bytes(a)
    return raw + (struct.pack(">I", license) if license is not None else b"")


XENIA_XUID = 0xE03000007EB97E5D
SAVE_BYTES = bytes(range(256)) * 3 + b"superman-save"


def make_xenia_tree(root: Path, packages: dict, xuid=XENIA_XUID):
    """packages: name -> header bytes (None = no header, "FILE" = STFS container file)."""
    base = root / ("%016X" % xuid) / "454107ED"
    for name, hdr in packages.items():
        pkg = base / "00000001" / name
        if hdr == "FILE":
            pkg.parent.mkdir(parents=True, exist_ok=True)
            pkg.write_bytes(b"CON " + b"\0" * 64)
            continue
        pkg.mkdir(parents=True)
        (pkg / name).write_bytes(SAVE_BYTES)
        if hdr is not None:
            hp = base / "Headers" / "00000001" / (name + ".header")
            hp.parent.mkdir(parents=True, exist_ok=True)
            hp.write_bytes(hdr)
    # A profile package like the user's real tree: must be ignored.
    prof = root / ("%016X" % xuid) / "FFFE07D1" / "00010000" / ("%016X" % xuid)
    prof.mkdir(parents=True, exist_ok=True)
    (prof / "Account").write_bytes(b"\0" * 16)


class LayoutTest(unittest.TestCase):
    def test_container_offsets_match_xenia_struct(self):
        m = XContentMetadata
        self.assertEqual(C.sizeof(XContentHeader), T.CON_HEADER_SIZE)
        self.assertEqual(C.sizeof(XContentMetadata), T.META_SIZE)
        self.assertEqual(C.sizeof(XContentContainerHeader), T.CONTAINER_SIZE)
        self.assertEqual(T.CONTAINER_FILE_SIZE, 0xA000)   # the user's real Xenia headers are 40960 bytes
        self.assertEqual(m.content_type.offset, T.M_CONTENT_TYPE)
        self.assertEqual(m.metadata_version.offset, T.M_METADATA_VERSION)
        self.assertEqual(m.execution_info.offset + ExecInfo.title_id.offset, T.M_EXEC_TITLE_ID)
        self.assertEqual(m.profile_id.offset, T.M_PROFILE_ID)
        self.assertEqual(m.display_name_raw.offset, T.M_DISPLAY_NAME)
        self.assertEqual(m.thumbnail_size.offset, T.M_THUMBNAIL_SIZE)
        self.assertEqual(m.thumbnail.offset, T.M_THUMBNAIL)
        self.assertEqual(m.display_name_ex_raw.offset, T.M_DISPLAY_NAME_EX)
        self.assertEqual(XContentHeader.licenses.offset, T.LICENSES_OFF)
        self.assertEqual(XContentContainerHeader.file_name_raw.offset, T.CON_FILE_NAME_OFF)
        # Free60 STFS absolute offsets, a third observer.
        self.assertEqual(T.META + T.M_DISPLAY_NAME, 0x411)
        self.assertEqual(T.META + T.M_EXEC_TITLE_ID, 0x360)
        self.assertEqual(T.META + T.M_THUMBNAIL_SIZE, 0x1712)

    def test_aggregate_offsets_match_port(self):
        a = XContentDataAggregate
        self.assertEqual(C.sizeof(a), T.AGG_SIZE)
        self.assertEqual(a.display_name_raw.offset, T.OFF_DISPLAY)
        self.assertEqual(a.file_name_raw.offset, T.OFF_FILE)
        self.assertEqual(a.padding.offset, T.OFF_PADDING)
        self.assertEqual(a.xuid.offset, T.OFF_XUID)
        self.assertEqual(a.title_id.offset, T.OFF_TITLE)

    def test_build_aggregate_equals_port_writer(self):
        # What writeHeaderLocked stores for the game's own save: XCONTENT_DATA from the
        # guest (device 1, type 1, names) with readXcd's xuid 0 and the resolved title.
        self.assertEqual(T.build_aggregate(T.GAME_DISPLAY_NAME, T.GAME_FILE_NAME), aggregate_header())


class ParseTest(unittest.TestCase):
    def test_container_english(self):
        i = T.parse_header(container_header())
        self.assertEqual((i.kind, i.display_name, i.file_name, i.content_type, i.title_id),
                         ("container", T.GAME_DISPLAY_NAME, T.GAME_FILE_NAME, 1, T.TITLE_ID))
        self.assertEqual(i.xuid, XENIA_XUID)

    def test_container_unpadded_form(self):   # 0x971A: header + metadata, no file name
        i = T.parse_header(container_header(pad_to_4k=False))
        self.assertEqual((i.kind, i.display_name, i.file_name), ("container", T.GAME_DISPLAY_NAME, ""))

    def test_container_fallback_language(self):
        # GetContentMetadata: English empty -> first filled of display_name(0..11).
        # display_name(0) reads slot 1 (kInvalid quirk); the fallback only runs when
        # slot 0 is empty, so the quirk is unobservable (an equivalent mutant).
        i = T.parse_header(container_header(display="Deutsch", lang_slot=2))
        self.assertEqual(i.display_name, "Deutsch")
        i = T.parse_header(container_header(display="ex", lang_slot=10))
        self.assertEqual(i.display_name, "ex")
        i = T.parse_header(container_header(display="ex", lang_slot=10, version=1))
        self.assertEqual(i.display_name, "")

    def test_aggregate(self):
        i = T.parse_header(aggregate_header(license=7))
        self.assertEqual((i.kind, i.display_name, i.license), ("aggregate", T.GAME_DISPLAY_NAME, 7))

    def test_invalid_size(self):
        self.assertEqual(T.parse_header(b"\0" * 0x200).kind, "invalid")
        self.assertEqual(T.parse_header(None).kind, "missing")


class PortRulesTest(unittest.TestCase):
    def test_port_rejects_container_as_is(self):
        # The reason conversion exists: readHeader() refuses anything but 0x148/0x14C.
        ok, why = T.port_accepts_header(container_header(), T.GAME_FILE_NAME)
        self.assertFalse(ok)
        self.assertIn("0x148", why)

    def test_rules(self):
        self.assertTrue(T.port_accepts_header(aggregate_header(), T.GAME_FILE_NAME)[0])
        self.assertTrue(T.port_accepts_header(aggregate_header(license=1), T.GAME_FILE_NAME)[0])
        self.assertFalse(T.port_accepts_header(aggregate_header(), "Other")[0])
        full = bytearray(aggregate_header())
        for k in range(128):
            struct.pack_into(">H", full, T.OFF_DISPLAY + 2 * k, 0x41)
        self.assertFalse(T.port_accepts_header(bytes(full), T.GAME_FILE_NAME)[0])
        wrong_type = bytearray(aggregate_header()); struct.pack_into(">I", wrong_type, 4, 2)
        self.assertFalse(T.port_accepts_header(bytes(wrong_type), T.GAME_FILE_NAME)[0])

    def test_valid_host_component(self):
        for good in ("Superman Returns", "a.b", "COM10"):
            self.assertTrue(T.valid_host_component(good), good)
        for bad in ("", ".", "..", "a ", "a.", "a:b", "CON", "nul.txt", "café", "x\x01"):
            self.assertFalse(T.valid_host_component(bad), bad)


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.src, self.dst = self.root / "xenia", self.root / "ours"

    def tearDown(self):
        self.tmp.cleanup()

    def run_import(self, **kw):
        out = io.StringIO()
        args = dict(src_root=self.src, src_xuid=None, dst_root=self.dst, dst_xuid=T.OUR_XUID,
                    force=False, dry_run=False, only=None, thumbnail=False, out=out)
        args.update(kw)
        rc, rec = T.run_import(**args)
        return rc, rec, out.getvalue()

    def dst_header(self, name=T.GAME_FILE_NAME) -> Path:
        return self.dst / "0009000000000001" / "454107ED" / "Headers" / "00000001" / (name + ".header")

    def dst_pkg(self, name=T.GAME_FILE_NAME) -> Path:
        return self.dst / "0009000000000001" / "454107ED" / "00000001" / name

    def test_current_xenia_save_converted(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()})
        rc, rec, out = self.run_import()
        self.assertEqual(rc, 0, out)
        hdr = self.dst_header().read_bytes()
        self.assertEqual(hdr, aggregate_header())          # byte-identical to the port's own writer
        self.assertTrue(T.port_accepts_header(hdr, T.GAME_FILE_NAME)[0])
        self.assertEqual((self.dst_pkg() / T.GAME_FILE_NAME).read_bytes(), SAVE_BYTES)
        self.assertFalse((self.dst / "E03000007EB97E5D").exists())
        self.assertEqual(rec[0]["source_xuid"], "E03000007EB97E5D")
        self.assertEqual(T.run_verify(self.dst, T.OUR_XUID, io.StringIO()), 0)

    def test_old_xenia_save_copied_verbatim(self):
        src_hdr = aggregate_header(xuid=XENIA_XUID, license=0)
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: src_hdr})
        rc, _, out = self.run_import()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.dst_header().read_bytes(), src_hdr)

    def test_licence_kept(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header(license_bits=0x21)})
        self.assertEqual(self.run_import()[0], 0)
        hdr = self.dst_header().read_bytes()
        self.assertEqual((len(hdr), struct.unpack_from(">I", hdr, 0x148)[0]), (0x14C, 0x21))

    def test_licence_needs_flags(self):   # ContentPackage: only entries with license_flags count
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header(license_bits=0x21, license_flags=0)})
        self.assertEqual(self.run_import()[0], 0)
        self.assertEqual(len(self.dst_header().read_bytes()), 0x148)

    def test_thumbnail_opt_in(self):
        png = b"\x89PNG\r\n\x1a\n" + b"x" * 40
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header(thumb=png)})
        self.assertEqual(self.run_import()[0], 0)
        self.assertFalse((self.dst_pkg() / "__thumbnail.png").exists())
        self.run_import(force=True, thumbnail=True)
        self.assertEqual((self.dst_pkg() / "__thumbnail.png").read_bytes(), png)

    def test_refuses_overwrite_then_backs_up(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()})
        self.assertEqual(self.run_import()[0], 0)
        (self.dst_pkg() / T.GAME_FILE_NAME).write_bytes(b"player's newer save")
        rc, _, out = self.run_import()
        self.assertEqual(rc, 2, out)
        self.assertEqual((self.dst_pkg() / T.GAME_FILE_NAME).read_bytes(), b"player's newer save")
        rc, _, out = self.run_import(force=True)
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.dst_pkg() / T.GAME_FILE_NAME).read_bytes(), SAVE_BYTES)
        backups = list((self.dst / T.BACKUP_DIR_NAME).rglob(T.GAME_FILE_NAME))
        self.assertTrue(any(b.is_file() and b.read_bytes() == b"player's newer save" for b in backups))
        self.assertTrue(list((self.dst / T.BACKUP_DIR_NAME).rglob("*.header")))

    def test_dry_run_writes_nothing(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()})
        rc, _, _ = self.run_import(dry_run=True)
        self.assertEqual(rc, 0)
        self.assertFalse(self.dst.exists())

    def test_bad_packages_skipped(self):
        make_xenia_tree(self.src, {
            T.GAME_FILE_NAME: container_header(),
            "Stfs": "FILE",
            "NoHeader": None,
            "WrongType": container_header(file_name="WrongType", content_type=2),
            "WrongTitle": container_header(file_name="WrongTitle", title=0x4D5307E6),
            "Mismatch": container_header(file_name="SomethingElse"),
            "Junk": b"\0" * 77,
        })
        rc, rec, out = self.run_import()
        self.assertEqual(rc, 0, out)
        names = sorted(r["name"] for r in rec)
        self.assertEqual(names, ["NoHeader", T.GAME_FILE_NAME])   # a header-less folder still copies
        self.assertFalse(self.dst_header("NoHeader").exists())
        for n in ("Stfs", "WrongType", "WrongTitle", "Mismatch", "Junk"):
            self.assertFalse(self.dst_pkg(n).exists(), n)
        self.assertIn("STFS container", out)
        self.assertEqual(T.run_verify(self.dst, T.OUR_XUID, io.StringIO()), 1)   # NoHeader flagged

    def test_display_name_mismatch_warned(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header(display="Renamed")})
        rc, rec, out = self.run_import()
        self.assertEqual(rc, 0)
        self.assertIn("will not match", out)

    def test_xuid_autodetect(self):
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()})
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()}, xuid=0xE030000007AFB800)
        rc, _, out = self.run_import()
        self.assertEqual(rc, 1)
        self.assertIn("--src-xuid", out)
        rc, rec, out = self.run_import(src_xuid=0xE030000007AFB800)
        self.assertEqual((rc, rec[0]["source_xuid"]), (0, "E030000007AFB800"))

    def test_no_saves_yet(self):   # the user's real Xenia tree today: profiles, no 454107ED saves
        (self.src / "E03000007EB97E5D" / "FFFE07D1" / "00010000").mkdir(parents=True)
        rc, _, out = self.run_import()
        self.assertEqual(rc, 1)
        self.assertIn("save in-game first", out)


class ProfileNameTest(unittest.TestCase):
    """iter-464: a save stores the profile name (MCM2 part1 +0, 16 bytes) and LOAD compares it with
    the signed-in name; an import rewrites it to the destination's gamertag (CRCs recomputed)."""

    @staticmethod
    def mcm2(name: bytes) -> bytes:
        sr = T._save_records()
        part1 = name.ljust(16, b"\0") + bytes(range(72))
        part2 = bytes((i * 7) & 0xFF for i in range(96 * 4))
        return sr.build(part1, part2)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.src, self.dst = self.root / "xenia", self.root / "ours"
        make_xenia_tree(self.src, {T.GAME_FILE_NAME: container_header()})
        f = next((self.src).rglob(T.GAME_FILE_NAME + "/" + T.GAME_FILE_NAME))
        f.write_bytes(self.mcm2(b"XeniaTag"))
        self.save = self.dst / "0009000000000001" / "454107ED" / "00000001" / T.GAME_FILE_NAME / T.GAME_FILE_NAME

    def tearDown(self):
        self.tmp.cleanup()

    def imp(self, **kw):
        out = io.StringIO()
        rc, rec = T.run_import(self.src, None, self.dst, T.OUR_XUID, False, False, None, False, out, **kw)
        return rc, rec, out.getvalue()

    def check_valid(self, data, name):
        sr = T._save_records()
        _h, p1, p2, chk = sr.split(data)
        self.assertTrue(all(chk.values()), chk)                      # CRCs recomputed
        self.assertEqual(p1[:16], name.encode().ljust(16, b"\0"))
        self.assertEqual(p1[16:], bytes(range(72)))                  # the rest of part1 untouched
        self.assertEqual(p2, bytes((i * 7) & 0xFF for i in range(96 * 4)))

    def test_default_player(self):
        rc, rec, out = self.imp()
        self.assertEqual(rc, 0, out)
        self.check_valid(self.save.read_bytes(), "Player")
        self.assertEqual(rec[0]["profile_names"], [{"file": T.GAME_FILE_NAME, "stored": "XeniaTag", "now": "Player"}])
        self.assertIn("profile name 'XeniaTag' -> 'Player'", out)

    def test_destination_gamertag(self):
        (self.dst / "settings").mkdir(parents=True)
        (self.dst / "settings" / "profile.ini").write_bytes(b"# x\r\ngamertag=Clark Kent \r\n")
        self.assertEqual(T.dest_gamertag(self.dst), "Clark Kent")      # the kernel's parse
        rc, _, out = self.imp()
        self.assertEqual(rc, 0, out)
        self.check_valid(self.save.read_bytes(), "Clark Kent")

    def test_explicit_and_keep(self):
        rc, _, out = self.imp(gamertag="Kal-El")
        self.assertEqual(rc, 0, out)
        self.check_valid(self.save.read_bytes(), "Kal-El")
        shutil.rmtree(self.dst)
        rc, rec, out = self.imp(gamertag="keep")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.save.read_bytes(), self.mcm2(b"XeniaTag"))   # byte-identical
        self.assertEqual(rec[0]["profile_names"], [])

    def test_bad_gamertag_refused(self):
        for bad in ("", "x" * 16, "tab\there"):
            rc, _, _ = self.imp(gamertag=bad)
            self.assertEqual(rc, 1, bad)
        self.assertFalse(self.dst.exists())

    def test_invalid_kernel_lines_fall_back(self):
        (self.dst / "settings").mkdir(parents=True)
        (self.dst / "settings" / "profile.ini").write_bytes(b" gamertag=Indented\ngamertag=" + b"y" * 16 + b"\n")
        self.assertEqual(T.dest_gamertag(self.dst), "Player")

    def test_library_snapshot_uses_default(self):
        out = io.StringIO()
        lib = self.root / "lib"
        self.assertEqual(T.library_add(lib, "named", self.src, None, "", False, False, out), 0, out.getvalue())
        snap_save = next((lib / "named").rglob(T.GAME_FILE_NAME + "/" + T.GAME_FILE_NAME))
        self.check_valid(snap_save.read_bytes(), "Player")


class LibraryTest(unittest.TestCase):
    def test_add_list_path_and_reimport_roundtrip(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            src, lib = t / "xenia", t / "lib"
            make_xenia_tree(src, {T.GAME_FILE_NAME: container_header()})
            out = io.StringIO()
            self.assertEqual(T.library_add(lib, "ch03-warworld", src, None, "before arena", False, False, out), 0)
            snap = lib / "ch03-warworld"
            meta = json.loads((snap / T.SNAPSHOT_FILE).read_text(encoding="utf-8"))
            self.assertEqual(meta["packages"][0]["name"], T.GAME_FILE_NAME)
            self.assertTrue(meta["seed"].startswith("SR_CONTENT_DIR="))
            self.assertEqual(T.run_verify(snap, T.OUR_XUID, io.StringIO()), 0)
            self.assertEqual(T.library_add(lib, "ch03-warworld", src, None, "", False, False, io.StringIO()), 2)
            self.assertEqual(T.library_add(lib, "bad label!", src, None, "", False, False, io.StringIO()), 1)
            lst = io.StringIO(); T.library_list(lib, lst)
            self.assertIn("ch03-warworld", lst.getvalue())
            # A snapshot is itself a content root: re-snapshotting it (our layout, 0x148
            # headers) is an identity copy - the same path snapshots a roll's end state.
            self.assertEqual(T.library_add(lib, "copy", snap, None, "", False, False, io.StringIO()), 0)
            a = (snap / "0009000000000001" / "454107ED" / "Headers" / "00000001" / "Superman Returns.header").read_bytes()
            b = (lib / "copy" / "0009000000000001" / "454107ED" / "Headers" / "00000001" / "Superman Returns.header").read_bytes()
            self.assertEqual(a, b)

    def test_devtool_marker_refused_unless_allowed(self):
        """iter-463 phase 2 (DEVTOOLS_DESIGN 3.2): a source with a .devtool taint marker is refused
        (exit 2, nothing written); --allow-devtool imports it, carries the marker and records
        origin devtool-derived."""
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            src, lib = t / "xenia", t / "lib"
            make_xenia_tree(src, {T.GAME_FILE_NAME: container_header()})
            xu = next(p for p in src.iterdir() if p.is_dir())
            pkg_dir = next(xu.rglob(T.GAME_FILE_NAME))
            (pkg_dir.parent / (T.GAME_FILE_NAME + ".devtool")).write_text("{}")
            self.assertEqual(T.library_add(lib, "tainted", src, None, "", False, False, io.StringIO()), 2)
            self.assertFalse((lib / "tainted").exists())
            rc, _ = T.run_import(src, None, t / "dst", T.OUR_XUID, False, False, None, False, io.StringIO())
            self.assertEqual(rc, 2)
            self.assertFalse((t / "dst").exists())
            out = io.StringIO()
            self.assertEqual(T.library_add(lib, "tainted", src, None, "", False, False, out, allow_devtool=True), 0,
                             out.getvalue())
            meta = json.loads((lib / "tainted" / T.SNAPSHOT_FILE).read_text(encoding="utf-8"))
            self.assertEqual(meta["origin"], "devtool-derived")
            self.assertEqual([p["name"] for p in meta["packages"]], [T.GAME_FILE_NAME])   # the marker is no package
            self.assertTrue((T.type_dir(lib / "tainted", T.OUR_XUID) / (T.GAME_FILE_NAME + ".devtool")).is_file())

    def test_failed_add_leaves_no_snapshot(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "xenia").mkdir()
            self.assertNotEqual(T.library_add(t / "lib", "empty", t / "xenia", None, "", False, False, io.StringIO()), 0)
            self.assertFalse((t / "lib" / "empty").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
