#!/usr/bin/env python3
"""Import Superman Returns saves from Xenia into the port, and keep a save library.

Debugging infrastructure (iter-463): the user plays to a chapter in Xenia, saves,
and this tool turns that save into a package the port loads, or into a named
snapshot a test roll can start from. It never touches guest memory and never
runs the game; it only copies host files.

Layouts (both are Xenia's; kernel/xam_content.cpp follows the older one):

    <root>/<XUID 16 hex>/<TITLE 454107ED>/<TYPE 00000001>/<file name>/...       package
    <root>/<XUID>/<TITLE>/Headers/<TYPE>/<file name>.header                     metadata

The .header formats differ, and that is the one thing that must be rewritten:

  * Old Xenia and the port: XCONTENT_AGGREGATE_DATA, exactly 0x148 bytes
    (+ an optional be u32 licence word = 0x14C). XCONTENT_DATA 0x134 (device @0,
    type @4, UTF-16BE display name @8, file name @0x108 [42], padding @0x132),
    be u64 xuid @0x138, be u32 title id @0x140.
  * Current Xenia Canary (xcontent_package_directory.cc, 2026): the whole
    XContentContainerHeader (STFS "CON " header 0x344 + XContentMetadata 0x93D6 +
    a Xenia-only file_name[42] @0x971A = 0x9744 bytes), zero-padded to 0xA000.

The port's readHeader() accepts only 0x148 / 0x14C, so a current-Xenia header
would be ignored and the package listed under its FOLDER name. The game matches
saves by DISPLAY NAME (ENGINE_MAP 6i: wcsncmp against
L"Superman Returns: The Videogame"), so without conversion the save is never
found. This tool converts it the way Xenia itself reads it
(ContentPackage::GetContentMetadata): device 1, type and title from the
metadata, the English display name (else the first filled language), the
Xenia file name (else the folder name). xuid @0x138 is written as 0, which is
what the port's own writer stores (readXcd zeroes it); the guest never sees it
(enumeration copies only the first 0x134 bytes).

Usage:
    python tools/import_xenia_saves.py import [--src DIR] [--src-xuid X] [--dest DIR] [--force] [--dry-run]
    python tools/import_xenia_saves.py library add LABEL [--src DIR] [--note TEXT] [--force]
    python tools/import_xenia_saves.py library list
    python tools/import_xenia_saves.py library path LABEL
    python tools/import_xenia_saves.py verify [ROOT]

DEV-TOOL TAINT (iter-463 phase 2, docs/DEVTOOLS_DESIGN.md 3.1/3.2): a save written by a
dev-tool-assisted run carries a `<package>.devtool` marker beside its package folder (the exe
writes it; kernel/xam_content.cpp). `import` and `library add` REFUSE a source root that holds
any marker (exit 2) unless --allow-devtool is given; with it the markers are carried over beside
the imported packages and SNAPSHOT.json records "origin": "devtool-derived". Such saves are
never natural seeds.

PROFILE NAME (iter-464): a save stores the name of the profile that wrote it (16 bytes at MCM2
part1 offset 0 = file offset 0x1C; SaveFlow sub_822B5828 -> sub_82164F08(*0x8296D8D4,
*0x8296F21C + 1)), and LOAD refuses a save whose name differs from the signed-in one ("another
gamer profile's save file"). The port's name is its gamertag (<save root>/settings/profile.ini
`gamertag=`, default "Player"; kernel/xam_profile.cpp XamUserGetName). `import` and `library add`
therefore rewrite the stored name of every imported save to the destination's gamertag
(--gamertag NAME to choose another, --gamertag keep to leave the bytes alone) and recompute the
container CRCs with the game's own procedure (tools/save_records.py build()).

A library snapshot is a complete content root. Start a roll from it with
    .\\tools\\run_recomp_x.ps1 -K 1 -Gates "SR_CONTENT_DIR=<path printed by 'library path'>"
(run_recomp_x.ps1 and sr_drive.ps1 treat SR_CONTENT_DIR as a seed: they copy it
into a fresh per-roll root and never write the snapshot). See docs/SAVES.md.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import functools
import hashlib
import json
import os
import re
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ── identities ───────────────────────────────────────────────────────────────
TITLE_ID = 0x454107ED                 # kernel/xam_content.hpp kXamContentTitleId
OUR_XUID = 0x0009000000000001         # kernel/xam_content.hpp kXamUser0Xuid
TYPE_SAVED_GAME = 1
DEVICE_HDD = 1                        # XamShowDeviceSelectorUI stub + Xenia GetContentMetadata
GAME_DISPLAY_NAME = "Superman Returns: The Videogame"   # off_82913628 (ENGINE_MAP 6i)
GAME_FILE_NAME = "Superman Returns"

# Xenia's content folder (next to its exe): --xenia-content, or SR_XENIA_CONTENT.
DEFAULT_XENIA_CONTENT = Path(os.environ.get("SR_XENIA_CONTENT", "xenia_content"))
REPO = Path(__file__).resolve().parent.parent
DEFAULT_LIBRARY = REPO / "archive" / "saves_library"
BACKUP_DIR_NAME = ".import_backup"    # not 16 hex digits: the port never enumerates it
SNAPSHOT_FILE = "SNAPSHOT.json"
DEFAULT_GAMERTAG = "Player"           # kernel/xam_profile.cpp xamProfileGamertag()
PROFILE_NAME_LEN = 16                 # sub_82164F08: memcpy(mgr + 8, name, min(strlen, 16))

# ── XCONTENT_AGGREGATE_DATA (port + old Xenia) ──────────────────────────────
AGG_SIZE = 0x148
AGG_SIZE_LICENSED = 0x14C
OFF_DEVICE, OFF_TYPE, OFF_DISPLAY, OFF_FILE, OFF_PADDING = 0x0, 0x4, 0x8, 0x108, 0x132
OFF_XUID, OFF_TITLE = 0x138, 0x140
DISPLAY_WCHARS = 128
FILE_NAME_MAX = 42

# ── XContentContainerHeader (current Xenia) ─────────────────────────────────
# Offsets derived from xcontent.h (pack(1)); test_import_xenia_saves.py rebuilds
# them independently with ctypes and checks every constant here.
CON_HEADER_SIZE = 0x344                        # XContentHeader
META = CON_HEADER_SIZE                         # XContentMetadata starts here
META_SIZE = 0x93D6
M_CONTENT_TYPE = 0x0
M_METADATA_VERSION = 0x4
M_EXEC_TITLE_ID = 0x10 + 0xC                   # execution_info (0x18) .title_id
M_PROFILE_ID = 0x2D
M_DISPLAY_NAME = 0xCD                          # 9 languages x 128 wchar
M_THUMBNAIL_SIZE = 0x13CE
M_THUMBNAIL = 0x13D6
M_DISPLAY_NAME_EX = 0x50D6                     # languages 10..12 (metadata_version >= 2)
LICENSES_OFF = 4 + 0x228                       # magic + signature
LICENSE_COUNT = 0x10
CON_FILE_NAME_OFF = CON_HEADER_SIZE + META_SIZE        # 0x971A, Xenia-only field
CONTAINER_SIZE = CON_FILE_NAME_OFF + FILE_NAME_MAX     # 0x9744
CONTAINER_FILE_SIZE = (CONTAINER_SIZE + 0xFFF) & ~0xFFF  # 0xA000 (ExtractContentHeader)
THUMB_V1, THUMB_V2 = 0x4000, 0x3D00
NUM_LANG_V1, NUM_LANG_V2 = 9, 12
LANG_ENGLISH = 1


def be16(b: bytes, o: int) -> int: return struct.unpack_from(">H", b, o)[0]
def be32(b: bytes, o: int) -> int: return struct.unpack_from(">I", b, o)[0]
def be64(b: bytes, o: int) -> int: return struct.unpack_from(">Q", b, o)[0]


def read_wide(b: bytes, o: int, count: int) -> str:
    """UTF-16BE up to the first NUL within `count` wchars."""
    out = []
    for i in range(count):
        c = be16(b, o + 2 * i)
        if c == 0:
            break
        out.append(c)
    return struct.pack(">%dH" % len(out), *out).decode("utf-16-be", errors="replace")


def cstr(b: bytes, o: int, n: int) -> str:
    raw = b[o:o + n]
    z = raw.find(b"\0")
    return (raw if z < 0 else raw[:z]).decode("latin-1")


# ── the port's acceptance rules (mirror of kernel/xam_content.cpp) ───────────
def port_trim(s: str) -> str:
    return s.strip(" \t\r\n")


_DOS_NAMES = {"CON", "PRN", "AUX", "NUL"} | {"COM%d" % i for i in range(1, 10)} | {"LPT%d" % i for i in range(1, 10)}


def valid_host_component(name: str) -> bool:
    """Mirror of the port's validHostComponent (kernel/xam_content.cpp): printable
    ASCII, no Windows-reserved character, no trailing space/dot, no DOS device name."""
    if not name or len(name) > 255 or name in (".", ".."):
        return False
    if any(ord(c) < 0x20 or ord(c) >= 0x7F or c in '<>:"/\\|?*' for c in name):
        return False
    if name[-1] in " .":
        return False
    return name.split(".", 1)[0].upper() not in _DOS_NAMES


def port_accepts_header(raw: bytes, folder: str, type_: int = TYPE_SAVED_GAME) -> tuple[bool, str]:
    """readHeader() + headerMatchesFolder(): exact size, display name NUL-terminated
    within 128 wchars, trim(file name) == folder, content type == listed type."""
    if len(raw) not in (AGG_SIZE, AGG_SIZE_LICENSED):
        return False, "size 0x%X (the port reads only 0x148 / 0x14C)" % len(raw)
    if not any(be16(raw, OFF_DISPLAY + 2 * i) == 0 for i in range(DISPLAY_WCHARS)):
        return False, "display name has no NUL within 128 wchars"
    fn = cstr(raw, OFF_FILE, FILE_NAME_MAX)
    if port_trim(fn) != folder:
        return False, "file name %r != folder %r" % (fn, folder)
    if be32(raw, OFF_TYPE) != type_:
        return False, "content type %d != %d" % (be32(raw, OFF_TYPE), type_)
    return True, "ok"


# ── header parsing / conversion ─────────────────────────────────────────────
@dataclass
class HeaderInfo:
    kind: str                      # "aggregate" | "container" | "missing" | "invalid"
    display_name: str = ""
    file_name: str = ""
    content_type: int = 0
    title_id: int = 0
    xuid: int = 0                  # aggregate xuid or container profile_id
    license: int = 0
    thumbnail: bytes = b""
    problem: str = ""


def container_display_name(h: bytes, language: int) -> str:
    """XContentMetadata::display_name(XLanguage) - lang 0 (kInvalid) reads slot 1."""
    lang_id = 1 if language == 0 else language - 1
    if lang_id >= NUM_LANG_V2:
        lang_id = LANG_ENGLISH - 1
    version = be32(h, META + M_METADATA_VERSION)
    if lang_id < NUM_LANG_V1:
        off = META + M_DISPLAY_NAME + lang_id * 256
    elif lang_id < NUM_LANG_V2 and version >= 2:
        off = META + M_DISPLAY_NAME_EX + (lang_id - NUM_LANG_V1) * 256
    else:
        return ""
    return read_wide(h, off, DISPLAY_WCHARS)


def parse_header(data: bytes | None) -> HeaderInfo:
    if data is None:
        return HeaderInfo("missing", problem="no .header file")
    n = len(data)
    if n in (AGG_SIZE, AGG_SIZE_LICENSED):
        return HeaderInfo(
            "aggregate",
            display_name=read_wide(data, OFF_DISPLAY, DISPLAY_WCHARS),
            file_name=cstr(data, OFF_FILE, FILE_NAME_MAX),
            content_type=be32(data, OFF_TYPE),
            title_id=be32(data, OFF_TITLE),
            xuid=be64(data, OFF_XUID),
            license=be32(data, AGG_SIZE) if n == AGG_SIZE_LICENSED else 0)
    # ContentPackageDirectory::ReadContentHeaderFile accepts exactly these two sizes.
    if n in (CON_HEADER_SIZE + META_SIZE, CONTAINER_FILE_SIZE):
        h = data + b"\0" * max(0, CONTAINER_SIZE - n)   # 0x971A form has no file name
        # ContentPackage::GetContentMetadata: English first, else first filled.
        name = container_display_name(h, LANG_ENGLISH)
        if not name:
            for i in range(NUM_LANG_V2):
                name = container_display_name(h, i)
                if name:
                    break
        lic = 0
        for i in range(LICENSE_COUNT):
            o = LICENSES_OFF + i * 0x10
            bits, flags = be32(h, o + 8), be32(h, o + 12)
            if flags:
                lic |= bits
        tsize = be32(h, META + M_THUMBNAIL_SIZE)
        tmax = THUMB_V1 if be32(h, META + M_METADATA_VERSION) == 1 else THUMB_V2
        info = HeaderInfo(
            "container",
            display_name=name,
            file_name=cstr(h, CON_FILE_NAME_OFF, FILE_NAME_MAX),
            content_type=be32(h, META + M_CONTENT_TYPE),
            title_id=be32(h, META + M_EXEC_TITLE_ID),
            xuid=be64(h, META + M_PROFILE_ID),
            license=lic,
            thumbnail=h[META + M_THUMBNAIL:META + M_THUMBNAIL + min(tsize, tmax)])
        if h[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            info.problem = "container magic %r is not CON/LIVE/PIRS" % h[:4]
        return info
    return HeaderInfo("invalid", problem="unrecognised .header size 0x%X" % n)


def build_aggregate(display_name: str, file_name: str, content_type: int = TYPE_SAVED_GAME,
                    title_id: int = TITLE_ID, license: int = 0) -> bytes:
    """The 0x148 (or licensed 0x14C) header the port's writeHeaderLocked would store."""
    raw = bytearray(AGG_SIZE)
    struct.pack_into(">I", raw, OFF_DEVICE, DEVICE_HDD)
    struct.pack_into(">I", raw, OFF_TYPE, content_type)
    units = struct.unpack(">%dH" % (len(display_name.encode("utf-16-be")) // 2),
                          display_name.encode("utf-16-be"))[:DISPLAY_WCHARS - 1]
    for i, u in enumerate(units):
        struct.pack_into(">H", raw, OFF_DISPLAY + 2 * i, u)
    fn = file_name.encode("latin-1")[:FILE_NAME_MAX]
    raw[OFF_FILE:OFF_FILE + len(fn)] = fn
    struct.pack_into(">Q", raw, OFF_XUID, 0)
    struct.pack_into(">I", raw, OFF_TITLE, title_id)
    if license:
        raw += struct.pack(">I", license)
    return bytes(raw)


# ── content-root walking ────────────────────────────────────────────────────
def hex16(v: int) -> str: return "%016X" % v
def hex8(v: int) -> str: return "%08X" % v


DEVTOOL_MARKER_EXT = ".devtool"


def devtool_markers(root: Path) -> list:
    """Every dev-tool taint marker under root (sorted), [] when root is not a directory."""
    root = Path(root)
    return sorted(root.rglob("*" + DEVTOOL_MARKER_EXT)) if root.is_dir() else []


def type_dir(root: Path, xuid: int) -> Path:
    return root / hex16(xuid) / hex8(TITLE_ID) / hex8(TYPE_SAVED_GAME)


def header_file(root: Path, xuid: int, name: str) -> Path:
    return root / hex16(xuid) / hex8(TITLE_ID) / "Headers" / hex8(TYPE_SAVED_GAME) / (port_trim(name) + ".header")


def find_xuids(root: Path) -> list[int]:
    out = []
    if not root.is_dir():
        return out
    for d in sorted(root.iterdir()):
        if d.is_dir() and re.fullmatch(r"[0-9A-Fa-f]{16}", d.name):
            if (d / hex8(TITLE_ID) / hex8(TYPE_SAVED_GAME)).is_dir():
                out.append(int(d.name, 16))
    return out


@dataclass
class Plan:
    name: str
    src_pkg: Path
    src_header: Path | None
    info: HeaderInfo
    header_out: bytes | None = None
    dst_pkg: Path | None = None
    dst_header: Path | None = None
    thumbnail_out: bytes = b""
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def ok(self) -> bool:
        return not self.errors


def plan_package(src_root: Path, src_xuid: int, name: str, dst_root: Path, dst_xuid: int,
                 thumbnail: bool) -> Plan:
    src_pkg = type_dir(src_root, src_xuid) / name
    hp = header_file(src_root, src_xuid, name)
    data = hp.read_bytes() if hp.is_file() else None
    p = Plan(name, src_pkg, hp if data is not None else None, parse_header(data))
    p.dst_pkg = type_dir(dst_root, dst_xuid) / name
    p.dst_header = header_file(dst_root, dst_xuid, name)
    info = p.info
    if not src_pkg.is_dir():
        p.errors.append("package is a file (STFS container), not a folder: unsupported - "
                        "the port mounts only folder packages")
        return p
    if len(name) > FILE_NAME_MAX or not valid_host_component(name):
        p.errors.append("folder name %r cannot round-trip through XCONTENT_DATA (<= 42 chars, "
                        "valid host component); the port skips it" % name)
        return p
    if info.kind == "missing":
        p.warnings.append("no .header: the port will list the save under its folder name %r, "
                          "which the game does not match (it looks for %r)" % (name, GAME_DISPLAY_NAME))
        return p
    if info.kind == "invalid":
        p.errors.append(info.problem)
        return p
    if info.problem:
        p.warnings.append(info.problem)
    if info.content_type != TYPE_SAVED_GAME:
        p.errors.append("header content type %d is not a saved game (1)" % info.content_type)
        return p
    if info.title_id not in (TITLE_ID, 0xFFFFFFFF):
        p.errors.append("header title id %08X is not Superman Returns (%08X)" % (info.title_id, TITLE_ID))
        return p
    file_name = info.file_name if info.file_name else name
    if port_trim(file_name) != name:
        p.errors.append("header file name %r does not match its folder %r" % (file_name, name))
        return p
    if info.kind == "aggregate":
        p.header_out = hp.read_bytes()          # already the port's format: copied byte for byte
    else:
        p.header_out = build_aggregate(info.display_name, file_name, info.content_type,
                                       TITLE_ID, info.license)
        if info.license:
            p.warnings.append("licence mask %08X kept as the 0x14C licence word" % info.license)
    ok, why = port_accepts_header(p.header_out, name)
    if not ok:
        p.errors.append("converted header would be rejected by the port: " + why)
        return p
    if info.display_name != GAME_DISPLAY_NAME:
        p.warnings.append("display name %r is not the game's %r: the game will not match this save"
                          % (info.display_name, GAME_DISPLAY_NAME))
    if thumbnail and info.kind == "container" and info.thumbnail[:8] == b"\x89PNG\r\n\x1a\n":
        if not (src_pkg / "__thumbnail.png").exists():
            p.thumbnail_out = info.thumbnail
    return p


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ── the stored profile name (iter-464) ─────────────────────────────────────
def valid_gamertag(s: str) -> bool:
    """The kernel's rule: 1..15 printable ASCII characters."""
    return 1 <= len(s) <= 15 and all(0x20 <= ord(c) < 0x7F for c in s)


def dest_gamertag(root: Path) -> str:
    """The gamertag the port signs in with for this save root - the same parse as
    kernel/xam_profile.cpp xamProfileGamertag(): the first valid `gamertag=` line, else Player."""
    try:
        text = (Path(root) / "settings" / "profile.ini").read_bytes().decode("latin-1")
    except OSError:
        return DEFAULT_GAMERTAG
    for line in text.split("\n"):
        if not line.startswith("gamertag="):
            continue
        v = line[9:].rstrip("\r \t")
        if valid_gamertag(v):
            return v
    return DEFAULT_GAMERTAG


@functools.lru_cache(maxsize=1)
def _save_records():
    import importlib.util
    spec = importlib.util.spec_from_file_location("save_records", Path(__file__).with_name("save_records.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def stored_profile_name(data: bytes) -> str | None:
    """The profile name in a game save (MCM2 part1 +0), or None if `data` is not a valid MCM2 file."""
    sr = _save_records()
    try:
        _hdr, p1, _p2, chk = sr.split(data)
    except ValueError:
        return None
    if not all(chk.values()) or len(p1) < PROFILE_NAME_LEN:
        return None
    return p1[:PROFILE_NAME_LEN].split(b"\0", 1)[0].decode("latin-1")


def rewrite_profile_name(data: bytes, name: str) -> bytes | None:
    """`data` with the stored profile name set to `name` (zero-padded to 16 bytes, as the game's
    zeroed buffer + memcpy leaves it) and the CRCs recomputed; None if not a valid MCM2 file."""
    sr = _save_records()
    try:
        _hdr, p1, p2, chk = sr.split(data)
    except ValueError:
        return None
    if not all(chk.values()):
        return None
    raw = name.encode("ascii")[:PROFILE_NAME_LEN]
    p1 = raw + b"\0" * (PROFILE_NAME_LEN - len(raw)) + p1[PROFILE_NAME_LEN:]
    return sr.build(p1, p2)


def retag_package(pkg: Path, gamertag: str, out=sys.stdout) -> list[dict]:
    """Rewrite the stored profile name of every game save in an imported package."""
    done = []
    for f in sorted(pkg.rglob("*")):
        if not f.is_file():
            continue
        data = f.read_bytes()
        old = stored_profile_name(data)
        if old is None:
            continue
        rec = {"file": str(f.relative_to(pkg)).replace("\\", "/"), "stored": old, "now": old}
        if old != gamertag:
            f.write_bytes(rewrite_profile_name(data, gamertag))
            rec["now"] = gamertag
            print("      profile name %r -> %r in %s (LOAD compares it with the port's gamertag)"
                  % (old, gamertag, rec["file"]), file=out)
        done.append(rec)
    return done


def run_import(src_root: Path, src_xuid: int | None, dst_root: Path, dst_xuid: int,
               force: bool, dry_run: bool, only: list[str] | None, thumbnail: bool,
               out=sys.stdout, allow_devtool: bool = False,
               gamertag: str | None = None) -> tuple[int, list[dict]]:
    """Returns (exit code, per-package records). 0 ok, 1 nothing/errors, 2 refused conflicts
    or a dev-tool-tainted source without allow_devtool."""
    src_root, dst_root = Path(src_root), Path(dst_root)
    if src_root.resolve() == dst_root.resolve():
        print("ERROR: source and destination are the same content root", file=out)
        return 1, []
    # the stored profile name follows the destination's gamertag (None = the destination's
    # settings/profile.ini, "keep" = leave the save bytes alone)
    if gamertag is None:
        gamertag = dest_gamertag(dst_root)
    if gamertag != "keep" and not valid_gamertag(gamertag):
        print("ERROR: --gamertag must be 1..15 printable ASCII characters (or 'keep')", file=out)
        return 1, []
    markers = devtool_markers(src_root)
    if markers and not allow_devtool:
        print("REFUSED: %s holds %d dev-tool taint marker(s) (e.g. %s): saves written by a "
              "dev-tool-assisted run (DEVTOOLS_DESIGN 3.1). Pass --allow-devtool to import them as "
              "devtool-derived; they are never natural seeds." % (src_root, len(markers), markers[0]), file=out)
        return 2, []
    xuids = find_xuids(src_root)
    if src_xuid is None:
        if len(xuids) != 1:
            print("ERROR: %s holds Superman Returns saves for %d profiles %s%s"
                  % (src_root, len(xuids), [hex16(x) for x in xuids],
                     "; pass --src-xuid" if xuids else ""), file=out)
            if not xuids:
                print("  (no <XUID>/%s/%s folder: sign in to a profile in Xenia and save in-game first)"
                      % (hex8(TITLE_ID), hex8(TYPE_SAVED_GAME)), file=out)
            return 1, []
        src_xuid = xuids[0]
    tdir = type_dir(src_root, src_xuid)
    if not tdir.is_dir():
        print("ERROR: no saves at %s" % tdir, file=out)
        return 1, []
    names = sorted(e.name for e in tdir.iterdir() if not e.name.endswith(DEVTOOL_MARKER_EXT))
    if only:
        names = [n for n in names if n in only]
    if not names:
        print("ERROR: no packages under %s" % tdir, file=out)
        return 1, []
    plans = [plan_package(src_root, src_xuid, n, dst_root, dst_xuid, thumbnail) for n in names]
    print("source  %s  (profile %s)" % (src_root, hex16(src_xuid)), file=out)
    print("dest    %s  (profile %s)%s" % (dst_root, hex16(dst_xuid), "  [dry run]" if dry_run else ""), file=out)
    records = []
    for p in plans:
        print("  %-24s header=%-9s display=%r" % (p.name, p.info.kind, p.info.display_name), file=out)
        for w in p.warnings:
            print("      warn: " + w, file=out)
        for e in p.errors:
            print("      SKIP: " + e, file=out)
    good = [p for p in plans if p.ok()]
    conflicts = [p for p in good if p.dst_pkg.exists() or p.dst_header.exists()]
    if conflicts and not force:
        for p in conflicts:
            print("REFUSED: %s already exists at the destination (pass --force; the old copy is "
                  "moved to %s/)" % (p.name, BACKUP_DIR_NAME), file=out)
        return 2, []
    if dry_run:
        return (0 if good else 1), [{"name": p.name, "ok": p.ok()} for p in plans]
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    for p in good:
        if p in conflicts:
            bk = dst_root / BACKUP_DIR_NAME / stamp
            for item in (p.dst_pkg, p.dst_header):
                if item.exists():
                    tgt = bk / item.relative_to(dst_root)
                    tgt.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(item), str(tgt))
                    print("  backup %s -> %s" % (item, tgt), file=out)
        p.dst_pkg.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(p.src_pkg, p.dst_pkg)
        if p.thumbnail_out:
            (p.dst_pkg / "__thumbnail.png").write_bytes(p.thumbnail_out)
        if p.header_out is not None:
            p.dst_header.parent.mkdir(parents=True, exist_ok=True)
            p.dst_header.write_bytes(p.header_out)
        names = retag_package(p.dst_pkg, gamertag, out) if gamertag != "keep" else []
        files = {str(f.relative_to(p.dst_pkg)).replace("\\", "/"): {"size": f.stat().st_size, "sha256": sha256(f)}
                 for f in sorted(p.dst_pkg.rglob("*")) if f.is_file()}
        # the taint travels with the save (allow_devtool only: otherwise refused above)
        src_marker = p.src_pkg.parent / (p.src_pkg.name + DEVTOOL_MARKER_EXT)
        tainted = src_marker.is_file()
        if tainted:
            shutil.copyfile(src_marker, p.dst_pkg.parent / (p.dst_pkg.name + DEVTOOL_MARKER_EXT))
        records.append({"name": p.name, "display_name": p.info.display_name,
                        "source_header": p.info.kind, "source_xuid": hex16(src_xuid),
                        "files": files, "warnings": p.warnings, "devtool_marker": tainted,
                        "profile_names": names})
        print("  imported %s (%d files)" % (p.name, len(files)), file=out)
    return (0 if good else 1), records


# ── verify ──────────────────────────────────────────────────────────────────
def run_verify(root: Path, xuid: int, out=sys.stdout) -> int:
    tdir = type_dir(root, xuid)
    if not tdir.is_dir():
        print("no saves for profile %s under %s" % (hex16(xuid), root), file=out)
        return 1
    bad = 0
    for e in sorted(tdir.iterdir()):
        hp = header_file(root, xuid, e.name)
        raw = hp.read_bytes() if hp.is_file() else None
        if not e.is_dir():
            print("  %-24s NOT A FOLDER (the port cannot mount it)" % e.name, file=out); bad += 1; continue
        if raw is None:
            print("  %-24s no header -> listed as %r (game will NOT match it)" % (e.name, e.name), file=out)
            bad += 1; continue
        ok, why = port_accepts_header(raw, e.name)
        if not ok:
            print("  %-24s header REJECTED by the port (%s) -> listed as %r" % (e.name, why, e.name), file=out)
            bad += 1; continue
        dn = read_wide(raw, OFF_DISPLAY, DISPLAY_WCHARS)
        match = dn == GAME_DISPLAY_NAME
        files = [f for f in e.rglob("*") if f.is_file()]
        print("  %-24s ok  display=%r%s  files=%d" % (e.name, dn, "" if match else "  (game will NOT match)",
                                                      len(files)), file=out)
        bad += 0 if match else 1
    return 0 if bad == 0 else 1


# ── library ─────────────────────────────────────────────────────────────────
LABEL_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")


def library_root(arg: str | None) -> Path:
    return Path(arg or os.environ.get("SR_SAVES_LIBRARY") or DEFAULT_LIBRARY)


def library_add(lib: Path, label: str, src_root: Path, src_xuid: int | None, note: str,
                force: bool, thumbnail: bool, out=sys.stdout, allow_devtool: bool = False,
                gamertag: str | None = None) -> int:
    if not LABEL_RE.fullmatch(label):
        print("ERROR: label must match %s" % LABEL_RE.pattern, file=out)
        return 1
    if devtool_markers(src_root) and not allow_devtool:
        print("REFUSED: %s holds dev-tool taint marker(s) (saves written by a dev-tool-assisted "
              "run). Pass --allow-devtool to snapshot them as devtool-derived." % src_root, file=out)
        return 2
    snap = lib / label
    if snap.exists():
        if not force:
            print("REFUSED: snapshot %r exists (pass --force; the old one is moved to %s/)"
                  % (label, BACKUP_DIR_NAME), file=out)
            return 2
        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        bk = lib / BACKUP_DIR_NAME / (label + "-" + stamp)
        bk.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(snap), str(bk))
        print("  backup %s -> %s" % (snap, bk), file=out)
    snap.mkdir(parents=True)
    # a snapshot is a fresh save root: its saves carry the default gamertag unless asked otherwise
    rc, records = run_import(src_root, src_xuid, snap, OUR_XUID, False, False, None, thumbnail, out,
                             allow_devtool=allow_devtool, gamertag=gamertag or DEFAULT_GAMERTAG)
    if rc != 0 or not records:
        shutil.rmtree(snap, ignore_errors=True)
        print("ERROR: nothing imported; snapshot %r not created" % label, file=out)
        return rc or 1
    meta = {"label": label, "created": _dt.datetime.now().isoformat(timespec="seconds"),
            "source": str(src_root), "note": note, "packages": records,
            "seed": 'SR_CONTENT_DIR=%s' % snap.resolve()}
    if any(r.get("devtool_marker") for r in records) or devtool_markers(src_root):
        meta["origin"] = "devtool-derived"
    (snap / SNAPSHOT_FILE).write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print("snapshot %r -> %s" % (label, snap.resolve()), file=out)
    print('roll from it: .\\tools\\run_recomp_x.ps1 -K 1 -Gates "SR_CONTENT_DIR=%s"' % snap.resolve(), file=out)
    return 0


def library_list(lib: Path, out=sys.stdout) -> int:
    if not lib.is_dir():
        print("(empty library %s)" % lib, file=out)
        return 0
    for d in sorted(lib.iterdir()):
        if not d.is_dir() or d.name == BACKUP_DIR_NAME:
            continue
        meta = {}
        try:
            meta = json.loads((d / SNAPSHOT_FILE).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        pk = ", ".join(p.get("name", "?") for p in meta.get("packages", [])) or "?"
        print("%-24s %s  [%s]  %s" % (d.name, meta.get("created", "?"), pk, meta.get("note", "")), file=out)
    return 0


def default_dest() -> Path:
    la = os.environ.get("LOCALAPPDATA")
    if not la:
        raise SystemExit("LOCALAPPDATA is not set; pass --dest")
    return Path(la) / "SupermanRecomp" / "content"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def src_args(p):
        p.add_argument("--src", default=os.environ.get("SR_XENIA_CONTENT", str(DEFAULT_XENIA_CONTENT)),
                       help="Xenia content folder (or any content root in this layout)")
        p.add_argument("--src-xuid", help="profile XUID folder under --src (auto if exactly one has saves)")
        p.add_argument("--gamertag", help="rewrite each save's stored profile name to this (default: the "
                       "destination's settings/profile.ini gamertag, else Player; 'keep' = leave it)")
        p.add_argument("--thumbnail", action="store_true",
                       help="also write the header's PNG thumbnail as __thumbnail.png (off: the game never reads it)")

    pi = sub.add_parser("import", help="copy saves into the port's content root")
    src_args(pi)
    pi.add_argument("--dest", help="content root (default %%LOCALAPPDATA%%/SupermanRecomp/content)")
    pi.add_argument("--xuid", default=hex16(OUR_XUID), help="destination profile XUID (the port's user 0)")
    pi.add_argument("--only", action="append", help="import only this package folder (repeatable)")
    pi.add_argument("--force", action="store_true", help="replace existing packages (old copy -> .import_backup/)")
    pi.add_argument("--dry-run", action="store_true")
    pi.add_argument("--allow-devtool", action="store_true",
                    help="import a source holding .devtool taint markers (dev-tool-assisted saves)")

    pl = sub.add_parser("library", help="named save snapshots for test rolls")
    lsub = pl.add_subparsers(dest="lcmd", required=True)
    la = lsub.add_parser("add"); la.add_argument("label"); src_args(la)
    la.add_argument("--note", default=""); la.add_argument("--force", action="store_true")
    la.add_argument("--allow-devtool", action="store_true",
                    help="snapshot a source holding .devtool taint markers (origin: devtool-derived)")
    la.add_argument("--library")
    ll = lsub.add_parser("list"); ll.add_argument("--library")
    lp = lsub.add_parser("path"); lp.add_argument("label"); lp.add_argument("--library")

    pv = sub.add_parser("verify", help="check a content root against the port's header rules")
    pv.add_argument("root", nargs="?")
    pv.add_argument("--xuid", default=hex16(OUR_XUID))

    a = ap.parse_args(argv)
    if a.cmd == "import":
        dest = Path(a.dest) if a.dest else default_dest()
        rc, _ = run_import(Path(a.src), int(a.src_xuid, 16) if a.src_xuid else None, dest,
                           int(a.xuid, 16), a.force, a.dry_run, a.only, a.thumbnail,
                           allow_devtool=a.allow_devtool, gamertag=a.gamertag)
        return rc
    if a.cmd == "verify":
        return run_verify(Path(a.root) if a.root else default_dest(), int(a.xuid, 16))
    lib = library_root(getattr(a, "library", None))
    if a.lcmd == "add":
        return library_add(lib, a.label, Path(a.src), int(a.src_xuid, 16) if a.src_xuid else None,
                           a.note, a.force, a.thumbnail, allow_devtool=a.allow_devtool,
                           gamertag=a.gamertag)
    if a.lcmd == "list":
        return library_list(lib)
    snap = lib / a.label
    if not snap.is_dir():
        print("no snapshot %r in %s" % (a.label, lib), file=sys.stderr)
        return 1
    print(snap.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
