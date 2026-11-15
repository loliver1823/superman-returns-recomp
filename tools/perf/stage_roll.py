#!/usr/bin/env python3
"""stage_roll.py - put a live roll's READ set on an SSD, so boot timing stops depending on the busy D: disk.

Owner 2026-10-14: "okay do them on j just be wary about space and resources". In about a quarter of rolls the boot
load (the 856-file installed shader cache, the DXC DLLs, the game image) took 6-100 s on D:, a 7200-rpm disk that
every worktree shares (devcreate-boot archive/dcb/FINDINGS.md). `sr_drive.ps1 -Stage <dir>` (perf_run.py /
flicker_gate.py / native_proof.py --stage) runs `stage` before the launch and starts the STAGED exe with the working
directory unchanged: the log, frames, the perf dir, saves and dumps stay where they are without -Stage.

What the game reads, and where it looks (the source code is the authority):
  exe folder    the exe (demand-paged during play), <stem>.pdb (dbghelp diagnostics), the DLLs beside it: SDL3,
                dxil + dxcompiler (gfx/shader_xlat_core.cpp dxcLoad: the exe folder unless SR_DXC_DIR), ffmpeg_xenia
                (kernel/ffmpeg_xenia.cpp); port_features/ (kernel/mod_manager.cpp loadPortFeatures: the exe folder
                first, then <content root>/data/port_features); data/fonts/saira (gfx/overlay_kit.cpp fontDir) and
                data/glyphs (gfx/glyph_subst.cpp init): the exe folder first, then the working directory's.
  the disc      "Superman Returns (USA)/": the content root is the first of the exe folder, its parent and the working
                directory that holds it (kernel/file_io.cpp contentRootForLocations); the image is
                <exe>/../Superman Returns (USA)/default.xex(.img) first (guest/guest_harness.cpp resolveXexPath). The
                source must hold default.xex, its image (default.xex.img or default.img) and the 12 retail DATA/*.AST
                files, with no link inside: otherwise the staged copy could leave the game reading D: (refused).
  shader cache  SR_SHADER_CACHE_DIR, else the exe folder's data/shader_cache/dxil, else the working directory's
                (gfx/shader_cache.cpp shippedDataDir; every file in that folder, not below it). Exes before
                render/shader-coverage (2026-10-17) looked in the working directory first; a dev build or a
                tools/snapshot_exe.ps1 copy has no cache beside it, so for those both orders pick the working
                directory's. The point-sprite GS is <that folder>/../point_sprite.gs.dxil (gfx/d3d12_backend.cpp
                pointSpriteGs; older exes: <SR_SHADER_CACHE_DIR>/.. or the working directory's); the stage takes the
                one beside the chosen folder, else the working directory's. Whichever folder the game would load, it
                is staged with that GS beside it and named by SR_SHADER_CACHE_DIR, so every exe reads the same.
  resolve kernels  the native resolve / texture kernels as bytecode, data/shader_cache/nb/<kernel>-<key>.dxbc
                (gfx/native_resolve.inc nbAotDir: SR_NB_AOT_DIR, else the exe folder's data/shader_cache/nb, else the
                working directory's; exes before render/shader-coverage looked in the working directory first). The key is the kernel source's hash, so the blobs are THE EXE'S: a set from
                another build misses, and the exe compiles the kernel at boot - in the release configuration on the
                CP's first command (s3m_gate2 2026-10-16 on the Ally: [GPU_HANG] H3 at 15.8 s; the Ally's checkout
                predated the exe's blobs). So the exe folder's own set (a snapshot made by tools/snapshot_exe.ps1
                carries an adjacent or explicitly selected set) is staged as nb/ in the shader entry and named by
                SR_NB_AOT_DIR; an explicit SR_NB_AOT_DIR wins. A checkout-only fallback is refused: select the
                build's source explicitly or make a complete new snapshot. No blob scope means no load claim.
So the staged exe folder sits one level below a staged content root, SR_SHADER_CACHE_DIR names the staged cache and
SR_NB_AOT_DIR the staged kernels.
Where the exe folder lacks port_features/, data/fonts/saira or data/glyphs, the copy the game would fall back to is
staged beside the staged exe (searched first), so the game reads the same bytes from the stage.
Outputs that stay where they are: the log and dumps (the working directory), SR_PERF_DIR, SR_PRESENT_DUMP_DIR, saves and
settings (SR_CONTENT_DIR, the attempt's own folder). Two exceptions: the game's generated port-feature layers
(<content root>/mods/.cache, ~20 MB) move to the stage with the content root; the runtime translator's per-process
scratch stays in %TEMP% (C:, pruned per roll). Not staged (small reads): input tapes, scenarios, any path an SR_*_DIR
gate names. Refused (exit 2): SR_MODS (the game reads mods/ from the content root) and SR_DEVTOOLS (plans are read
from, and devtools_out written under, the content root) - except the preset warp: SR_DEVTOOLS=cheats exactly with
SR_DEVTOOLS_WARP / SR_DEVTOOLS_WARP_ON_MARK, which reads and writes nothing there on an automated roll; such a roll is
recorded as dev-tool-assisted (stage.json dev_tools / run_class, a note line). Any other SR_DEVTOOLS_* key is refused.

Layout (nothing outside <stage> is ever touched; no link is followed, written through or deleted through):
  <stage>/SR_STAGE_ROOT.txt                   the marker: a non-empty folder without it is refused, never written
  <stage>/stage.lock                           the stage lock: an OS byte-range lock on an open handle (msvcrt.locking),
                                               released by the OS when its holder exits - there is no stale lock
  <stage>/content-<k>/Superman Returns (USA)/ the disc;  <stage>/content-<k>/exe-<sha12>-<k>/  the exe folder
  <stage>/shader-<k>/dxil/ + point_sprite.gs.dxil + nb/ (the resolve kernels, SR_NB_AOT_DIR)
  <entry>/STAGED.json                          per-file sha256 digests, written last
  <stage>/digests.json                         the DISC's source sha256 by (path, size, mtime)
  <stage>/leases/<id>.json                     the entries a driver holds until it finishes
CONTENT-ADDRESSED: every entry is named by sha256 over (relative path, size, sha256 of the bytes); a copy is hashed as
it is written and refused on a mismatch. The disc's source digests are cached by path + size + mtime (a read-only
retail disc: a source edited in place under the same size and mtime would keep its cached digest - the one blind
spot); the mutable sources (the exe folder, its DLLs, port features, the shader cache) are re-hashed on every
--verify full stage. --verify full (sr_drive) also re-hashes every staged file before reuse; --verify quick
(perf_run's pre-stage, followed by sr_drive's full pass) uses cached source digests and trusts a staged file whose size
and mtime match its manifest. A second roll with the same exe and data copies nothing. Stray files are removed.

LEASES: --lease-id <id> --lease-pid <pid> writes <stage>/leases/<id>.json under the stage lock before it is released:
it covers the selected entries while ANY of its holders runs (pid + creation time; `lease-add` adds one - sr_drive
adds each game it launches, so a killed driver leaves its game protected), until `release`, or after --lease-ttl-h
(default 12). clean and the stage's own retention never touch a leased entry. `release --handoff-pid P` keeps only P
(sr_drive -KeepAlive). Residual gap: a driver killed between a launch and its lease-add leaves that game unprotected.

SAFETY: every metadata and temporary file is created with O_EXCL after its folder is checked (never through an existing
name, never truncating; a leftover regular .part is removed, a link refused); stage and clean refuse any junction /
symlink in the stage, its root or an ancestor before changing anything; deletion goes file by file (os.scandir without
following links; a reparse point at any level stops it; each file's resolved path is re-checked inside the stage just
before its unlink; only empty folders are removed). Residual race: a folder swapped for a junction in the microseconds
between that check and the unlink - only this tool's own processes write under the stage, so not a realistic threat.

    python tools/perf/stage_roll.py plan    --stage J:\\sr_stage --exe <exe> [--root <repo>] [--source <dir>] [--files]
    python tools/perf/stage_roll.py stage   --stage J:\\sr_stage --exe <exe> [--root ..] [--source ..] [--json f]
                                            [--verify full|quick] [--lease-id ID --lease-pid PID]
    python tools/perf/stage_roll.py lease-add --stage J:\\sr_stage --lease-id ID --pid P
    python tools/perf/stage_roll.py release --stage J:\\sr_stage --lease-id ID [--handoff-pid P]
    python tools/perf/stage_roll.py status  --stage J:\\sr_stage
    python tools/perf/stage_roll.py clean   --stage J:\\sr_stage [--keep-exes N] [--dry-run]
    python tools/perf/stage_roll.py check   <perf_run --out | sr_drive -Out>   # where did the roll read from?

--root is the working directory the game runs in (sr_drive's repo root; default: the current directory). --source
is the folder holding "Superman Returns (USA)" to stage from (default: the game's own rule over the exe folder, its
parent and --root); it lets a lane worktree without the disc roll its own tools.

SPACE ("be wary about space and resources"): `stage` prints what it copies, the staged set's size and the stage
volume's free space, and refuses (exit 2; no STAGE_OK, no lease) when the free space would drop below --floor-gib
(default 50 GiB): before anything is created, before every file it copies, and once more after copying. The set may
not exceed --max-gib (20). Retention keeps the --keep-exes most recently used exe folders (3), 3 shader caches and
the current content root. `clean` removes every unleased entry and, holding the stage lock, the marker; then the lock
file and the stage folder (left in place when another stage_roll has just opened it: a folder holding only stage.lock
is an empty stage). `--keep-exes N` only trims exe folders.

CHECK grades each scope by positive witnesses and says INSUFFICIENT EVIDENCE where the run's logs cannot supply one:
exe image (provenance, perf meta.txt, perf memmap), shader cache ([SHADER_CACHE] loaded), overlays ([MOD] overlay
adds, port features on), content root (Content root / overlays), disc opens (a SUCCESSFUL NtCreateFile: "-> handle=";
FILE_NOT_FOUND and alias lines are no witness, an alias's target is), game image (xex_loader), DXC (perf memmap module
paths), glyphs ([GLYPH] ... from). Any path line naming a disc file outside the stage FAILs. The dev log (perf_run
--config port --gates SR_LOG=dev) with perf_run's SR_PERF_STATS witnesses every scope but fonts; no log line names
the font folder in any profile, so fonts are always INSUFFICIENT EVIDENCE. The player log lacks disc opens, the game
image and glyphs. Exit codes: 0 ok / PASS, 1 error / FAIL, 2 refused / NOT STAGED, 3 INSUFFICIENT EVIDENCE.
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import datetime
import glob
import hashlib
import json
import math
import os
import re
import shutil
import stat
import sys
import time

DISC = "Superman Returns (USA)"
DISC_REQUIRED = ("default.xex",) + tuple("DATA/" + n for n in (
    "_workinp.AST", "baseworl.AST", "fmv.AST", "fmvlegal.AST", "lunits.AST", "metropol.AST", "objtiv.AST",
    "preload.AST", "sound.AST", "text.AST", "ui.AST", "voice.AST"))
DISC_IMAGE = ("default.xex.img", "default.img")       # kernel/xex_loader.cpp: <xex>.img, then <stem>.img
MARKER = "SR_STAGE_ROOT.txt"
MANIFEST = "STAGED.json"
LOCK = "stage.lock"
LEASES = "leases"
DIGESTS = "digests.json"
VERSION = 2
GIB = 1024 ** 3
FLOOR_GIB = 50.0
MAX_GIB = 20.0
RESERVE = 256 * 1024 ** 2        # the game's own writes under the staged content root (mods/.cache, ~20 MB today)
KEEP_EXES = 3
KEEP_SHADERS = 3
LEASE_TTL_H = 12.0
CHUNK = 4 << 20
FONT_DIR = ("data", "fonts", "saira")                 # gfx/overlay_kit.cpp fontDir probes SairaSR-Medium.ttf
FONT_PROBE = "SairaSR-Medium.ttf"
GLYPH_DIR = ("data", "glyphs")                        # gfx/glyph_subst.cpp init opens glyph_map.txt
GLYPH_PROBE = "glyph_map.txt"
GS_REL = ("data", "shader_cache", "point_sprite.gs.dxil")


class Refused(Exception):
    """A refusal: printed, exit 2."""


class File:
    __slots__ = ("rel", "src", "size", "mtime", "digest")

    def __init__(self, rel, src, size, mtime):
        self.rel, self.src, self.size, self.mtime, self.digest = rel, src, size, mtime, None


# ------------------------------------------------------------------------------------------------ small helpers

def human(n):
    if n < 1000:
        return "%d B" % n
    for unit, div in (("KB", 1e3), ("MB", 1e6), ("GB", 1e9)):
        if n < div * 1000 or unit == "GB":
            return "%.2f %s" % (n / div, unit)
    return "%d B" % n


def gib(n):
    return "%.1f GiB" % (n / GIB)


def now():
    return datetime.datetime.now().isoformat(timespec="microseconds")   # last_used orders the retirements


def norm(p):
    return os.path.normcase(os.path.normpath(os.path.abspath(p)))


def under(child, parent):
    c, p = norm(child), norm(parent)
    return c == p or c.startswith(p.rstrip("\\/") + os.sep)


def under_resolved(child, parent):
    return under(os.path.realpath(child), os.path.realpath(parent))


def volume(p):
    return os.path.splitdrive(os.path.abspath(p))[0].upper() or p


def is_link_stat(st):
    if stat.S_ISLNK(st.st_mode):
        return True
    return bool(getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def is_link(path):
    try:
        return is_link_stat(os.stat(path, follow_symlinks=False))
    except OSError:
        return False


def links_in(path):
    """Every junction / symlink at or under path, never followed."""
    if is_link(path):
        return [path]
    if not os.path.isdir(path):
        return []
    out, stack = [], [path]
    while stack:
        d = stack.pop()
        for e in os.scandir(d):
            if is_link(e.path):
                out.append(e.path)
            elif e.is_dir(follow_symlinks=False):
                stack.append(e.path)
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def read_json(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def check_parent(path, root=None):
    """path's folder exists, is no junction / symlink and (root) resolves inside root."""
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent) or is_link(parent):
        raise Refused("%s: the folder is missing or is a junction / symlink - nothing is written there" % parent)
    if root and not under_resolved(parent, root):
        raise Refused("%s resolves outside the stage %s - nothing is written there" % (parent, root))


def create_excl(path, root=None):
    """A new file at path, open for writing, created with O_EXCL: never through an existing name, never truncating.
    A leftover regular file there (a .part from a crash) is removed first; a link or a folder is refused."""
    check_parent(path, root)
    for _attempt in (0, 1, 2):
        try:
            return os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o644)
        except (FileExistsError, PermissionError):    # Windows reports an existing folder / junction as EACCES
            try:
                st = os.stat(path, follow_symlinks=False)
            except FileNotFoundError:
                raise RuntimeError("cannot create %s (permission denied)" % path)
            if is_link_stat(st) or not stat.S_ISREG(st.st_mode):
                raise Refused("%s exists as a junction / symlink / folder - nothing is written through it" % path)
            os.unlink(path)       # a regular file: (if it were swapped for a link now, unlink removes the link)
    raise RuntimeError("%s keeps reappearing" % path)


def write_json(path, doc, root=None):
    """Write doc through <path>.part created with O_EXCL, then replace path (a rename replaces a name, never writes
    through it)."""
    tmp = path + ".part"
    with os.fdopen(create_excl(tmp, root), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    os.replace(tmp, path)


def free_bytes(path):
    p = os.path.abspath(path)
    while not os.path.isdir(p):
        parent = os.path.dirname(p)
        if parent == p:
            break
        p = parent
    return shutil.disk_usage(p).free


def _k32():
    import ctypes
    return ctypes, ctypes.WinDLL("kernel32", use_last_error=True)


def pid_alive(pid):
    if not pid or pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    ctypes, k32 = _k32()   # os.kill(pid, 0) TERMINATES the process on Windows: never use it there
    h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ctypes.get_last_error() == 5   # access denied: it exists
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == 259     # STILL_ACTIVE


def proc_created(pid):
    """The process's creation time (FILETIME ticks), or None: with the pid it names one process, not a reused pid."""
    if os.name != "nt" or not pid:
        return None
    ctypes, k32 = _k32()
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    t = [ctypes.c_ulonglong() for _ in range(4)]
    ok = k32.GetProcessTimes(h, *[ctypes.byref(x) for x in t])
    k32.CloseHandle(h)
    return int(t[0].value) if ok else None


# ------------------------------------------------------------------------------------------------ the read set

def scan_tree(src, prefix):
    """Every file under src as File(prefix + rel); a link inside is refused (it cannot be staged faithfully)."""
    out = []
    for dirpath, dirnames, filenames in os.walk(src):
        for n in sorted(dirnames + filenames):
            if is_link(os.path.join(dirpath, n)):
                raise Refused("a junction / symlink inside the source: %s" % os.path.join(dirpath, n))
        dirnames.sort()
        for n in sorted(filenames):
            p = os.path.join(dirpath, n)
            st = os.stat(p, follow_symlinks=False)
            if not stat.S_ISREG(st.st_mode):
                raise Refused("not a regular file inside the source: %s" % p)
            out.append(File(prefix + os.path.relpath(p, src).replace("\\", "/"), p, st.st_size, st.st_mtime_ns))
    return out


def one_file(rel, path):
    if is_link(path):
        raise Refused("a symlink in the read set: %s" % path)
    st = os.stat(path)
    return File(rel, path, st.st_size, st.st_mtime_ns)


def flat_files(src, prefix):
    """The regular files directly in src (ensureLoaded iterates the folder itself, not below it)."""
    out = []
    for n in sorted(os.listdir(src)):
        p = os.path.join(src, n)
        if is_link(p):
            raise Refused("a junction / symlink inside the shader cache: %s" % p)
        if os.path.isfile(p):
            out.append(one_file(prefix + n, p))
    return out


def content_root_of(exe, root):
    """kernel/file_io.cpp contentRootForLocations: the exe folder, its parent, the working directory."""
    d = os.path.dirname(os.path.abspath(exe))
    for c in (d, os.path.dirname(d), root):
        if c and os.path.isdir(os.path.join(c, DISC)):
            return os.path.abspath(c)
    return None


def disc_files(src):
    """The disc's files, validated: the image, default.xex and the retail DATA set present and non-empty."""
    files = scan_tree(os.path.join(src, DISC), DISC + "/")
    have = {f.rel[len(DISC) + 1:].lower(): f for f in files}
    missing = [r for r in DISC_REQUIRED if not have.get(r.lower()) or not have[r.lower()].size]
    if not any(have.get(i) and have[i].size for i in DISC_IMAGE):
        missing.append(" or ".join(DISC_IMAGE))
    if missing:
        raise Refused("the disc at %s is incomplete (missing or empty: %s): a staged copy would leave the game reading "
                      "D: or no disc at all" % (os.path.join(src, DISC), ", ".join(missing)))
    return files


def shader_source(root, exe, env):
    """-> (the cache folder the game would load, the point-sprite GS to stage beside it or None, why) or
    (None, None, why). The game's order (gfx/shader_cache.cpp shippedDataDir, render/shader-coverage): the exe
    folder's own cache first, the working directory's as the fallback. The GS is the one beside the chosen folder
    (gfx/d3d12_backend.cpp pointSpriteGs), else the working directory's (where exes before that change read it)."""
    e = env.get("SR_SHADER_CACHE_DIR")
    if e:
        d = os.path.abspath(os.path.join(root, e))
        return d, os.path.join(os.path.dirname(os.path.normpath(d)), "point_sprite.gs.dxil"), "SR_SHADER_CACHE_DIR"
    cwd_gs = os.path.join(root, *GS_REL)

    def gs_for(d):
        g = os.path.join(os.path.dirname(os.path.normpath(d)), "point_sprite.gs.dxil")
        return g if os.path.isfile(g) else cwd_gs
    besides = os.path.join(os.path.dirname(os.path.abspath(exe)), "data", "shader_cache", "dxil")
    if os.path.isdir(besides):
        return besides, gs_for(besides), "the exe folder's data/shader_cache/dxil (the exe's own cache)"
    p = os.path.join(root, "data", "shader_cache", "dxil")
    if os.path.isdir(p):
        return p, gs_for(p), "the working directory's data/shader_cache/dxil (none beside the exe)"
    return None, None, "no installed shader cache beside the exe or in the working directory"


NB_REL = ("data", "shader_cache", "nb")


def nb_source(root, exe, env):
    """-> (the resolve-kernel blob folder to stage, why) or (None, why). The blobs are keyed by the kernel source
    (gfx/native_resolve.inc nbAotCode), so they belong to ONE build: the exe folder's own set (tools/snapshot_exe.ps1
    carries it) is used; an unrelated working-directory fallback is refused. SR_NB_AOT_DIR set by the
    caller is what gets staged (as SR_SHADER_CACHE_DIR is). The game's own order is SR_NB_AOT_DIR, beside the exe,
    then the working directory (exes before render/shader-coverage: the working directory first) - the stage names
    the staged folder by SR_NB_AOT_DIR, so the game reads exactly what was staged."""
    e = env.get("SR_NB_AOT_DIR")
    if e:
        return os.path.abspath(os.path.join(root, e)), "SR_NB_AOT_DIR"
    mine = os.path.join(os.path.dirname(os.path.abspath(exe)), *NB_REL)
    if os.path.isdir(mine):
        return mine, "the exe folder's data/shader_cache/nb (the exe's own set)"
    cwd = os.path.join(root, *NB_REL)
    if os.path.isdir(cwd):
        raise Refused("the exe has no adjacent native blobs; refusing unrelated working-directory blobs at %s: "
                      "make a new snapshot with its build's blobs or explicitly set SR_NB_AOT_DIR" % cwd)
    return None, "no resolve-kernel blobs beside the exe or in the working directory: the exe compiles them at boot"


def exe_folder_files(exe, content_src, root):
    """The exe folder's read set -> (files, notes). Where the exe folder lacks something the game falls back to
    elsewhere, that fallback is staged beside the staged exe, which the game searches first. The exe folder's own
    data/shader_cache is not part of it: the shader entry stages whichever cache the game would load."""
    d = os.path.dirname(os.path.abspath(exe))
    name = os.path.basename(exe)
    stem = os.path.splitext(name)[0].lower()
    files = [one_file(name, os.path.abspath(exe))]
    notes = []
    for n in sorted(os.listdir(d)):
        p = os.path.join(d, n)
        low = n.lower()
        if n == name or not (low.endswith(".dll") or low == stem + ".pdb" or low == "portable.txt"):
            continue
        if os.path.isfile(p) or is_link(p):
            files.append(one_file(n, p))
    if os.path.isdir(os.path.join(d, "port_features")):
        files += scan_tree(os.path.join(d, "port_features"), "port_features/")
    elif content_src and os.path.isdir(os.path.join(content_src, "data", "port_features")):
        src = os.path.join(content_src, "data", "port_features")
        files += scan_tree(src, "port_features/")
        notes.append("port_features/ staged from %s (the exe folder has none; the game would read it there)" % src)
    for parts, probe in ((FONT_DIR, FONT_PROBE), (GLYPH_DIR, GLYPH_PROBE)):
        sub = "/".join(parts) + "/"
        mine, cwd = os.path.join(d, *parts), os.path.join(root, *parts)
        if os.path.isfile(os.path.join(mine, probe)):
            files += scan_tree(mine, sub)
        elif os.path.isfile(os.path.join(cwd, probe)):
            files += scan_tree(cwd, sub)
            notes.append("%s staged from %s (the exe folder has none; the game would read it there)" % (sub, cwd))
    return files, notes


# 2026-10-16 (tools/stage-devtools-allow, the orchestrator's decision for p2.zpd's warp probe): a staged roll may carry
# the PRESET WARP and nothing else of the dev tools - SR_DEVTOOLS=cheats exactly, with SR_DEVTOOLS_WARP (the point(s),
# "X Y Z" or "X Y Z;X Y Z;...") and SR_DEVTOOLS_WARP_ON_MARK (0 / 1: fire once at the tape's `mark dump`). On an
# automated roll the cheats tool reads no plan and writes nothing under the content root: kernel/devtools.cpp applies
# the content-root defaults (<root>/levelskip.txt, <root>/devtools_out) only to a dev.txt source, and the plan folder
# <root>/data/devtools/plans is read only by the level-skip menu. Every other tool (levelskip, objstate, freecam, ...,
# a list, @dev.txt) and every other SR_DEVTOOLS_* key (SR_DEVTOOLS_OUT, SR_DEVTOOLS_CMDS, ...) stays refused. A staged
# roll that carries the warp is DEV-TOOL-ASSISTED: the stage record names the keys (devtools_record).
DEVTOOLS_STAGED_TOOL = "cheats"
DEVTOOLS_STAGED_KEYS = ("SR_DEVTOOLS_WARP", "SR_DEVTOOLS_WARP_ON_MARK")
WARP_POINT = re.compile(r"\s*[-+]?\d+(\.\d+)?\s+[-+]?\d+(\.\d+)?\s+[-+]?\d+(\.\d+)?\s*", re.ASCII)


def valid_warp_point(point):
    if not WARP_POINT.fullmatch(point):
        return False
    # The guest warp takes float coordinates. Decimal text may match the grammar
    # while overflowing that representation; do not stage a silently invalid tape.
    return all(math.isfinite(float(value)) and abs(float(value)) <= 3.4028234663852886e38
               for value in point.split())


def devtools_record(env):
    """The dev-tool keys a roll carries ({} for none): SR_DEVTOOLS when set to a tool, every non-empty SR_DEVTOOLS_*."""
    rec = {k: v for k, v in env.items() if k.startswith("SR_DEVTOOLS_") and v not in (None, "")}
    if env.get("SR_DEVTOOLS") not in (None, "", "0"):
        rec["SR_DEVTOOLS"] = env["SR_DEVTOOLS"]
    return dict(sorted(rec.items()))


def gate_refusals(env):
    out = []
    m = env.get("SR_MODS")
    if m not in (None, "", "0"):
        out.append("SR_MODS=%s: the game reads mods/ from the content root, and the staged content root has none" % m)
    t = env.get("SR_DEVTOOLS")
    if t not in (None, "", "0") and t != DEVTOOLS_STAGED_TOOL:
        out.append("SR_DEVTOOLS=%s: only SR_DEVTOOLS=%s (the preset warp) runs staged - other dev tools read "
                   "data/devtools/plans from, or write devtools_out under, the content root" % (t, DEVTOOLS_STAGED_TOOL))
    for k in sorted(env):
        v = env[k]
        if not k.startswith("SR_DEVTOOLS_") or v in (None, ""):
            continue
        if k not in DEVTOOLS_STAGED_KEYS:
            out.append("%s=%s: of the SR_DEVTOOLS_* keys only %s run staged" % (k, v, " / ".join(DEVTOOLS_STAGED_KEYS)))
        elif t != DEVTOOLS_STAGED_TOOL:
            out.append("%s=%s without SR_DEVTOOLS=%s: the preset warp is the cheats tool's" % (k, v, DEVTOOLS_STAGED_TOOL))
    w = env.get("SR_DEVTOOLS_WARP")
    if w not in (None, ""):
        pts = w.split(";")
        if not pts or not all(valid_warp_point(p) for p in pts):
            out.append("SR_DEVTOOLS_WARP=%s: not \"X Y Z\" or \"X Y Z;X Y Z;...\"" % w)
    om = env.get("SR_DEVTOOLS_WARP_ON_MARK")
    if om not in (None, "", "0", "1"):
        out.append("SR_DEVTOOLS_WARP_ON_MARK=%s: 0 or 1" % om)
    return out


# ------------------------------------------------------------------------------------------------ digests

class Digests:
    """<stage>/digests.json: a source file's sha256 by (path, size, mtime). Trusted for the read-only retail disc; the
    mutable sources (exe folder, shader cache) use it only under --verify quick, followed by sr_drive's full pass."""

    def __init__(self, stage):
        self.stage = stage
        self.path = os.path.join(stage, DIGESTS)
        doc = read_json(self.path) if os.path.isfile(self.path) and not is_link(self.path) else None
        self.map = (doc or {}).get("files", {}) if (doc or {}).get("version") == VERSION else {}
        self.dirty = False

    def get(self, f):
        v = self.map.get(norm(f.src))
        return v[2] if v and v[0] == f.size and v[1] == f.mtime else None

    def put(self, f, digest):
        self.map[norm(f.src)] = [f.size, f.mtime, digest, int(time.time())]
        self.dirty = True

    def fill(self, files, hash_missing, use_cache=True):
        """Set f.digest from the cache (use_cache); hash the rest (sequentially: the sources sit on a busy hard disk)
        when hash_missing. -> (files hashed, bytes hashed, files still unknown)."""
        n = b = unknown = 0
        for f in files:
            f.digest = self.get(f) if use_cache else None
            if f.digest:
                continue
            if not hash_missing:
                unknown += 1
                continue
            d = sha256_file(f.src)
            st = os.stat(f.src)
            if st.st_size != f.size or st.st_mtime_ns != f.mtime:
                raise RuntimeError("%s changed while it was being hashed" % f.src)
            f.digest = d
            self.put(f, d)
            n += 1
            b += f.size
        return n, b, unknown

    def save(self):
        if self.dirty and os.path.isdir(os.path.dirname(self.path)):
            if len(self.map) > 50000:   # bounded: keep the most recently hashed
                keep = sorted(self.map.items(), key=lambda kv: kv[1][3], reverse=True)[:40000]
                self.map = dict(keep)
            write_json(self.path, {"version": VERSION, "files": self.map}, self.stage)
            self.dirty = False


def content_key(tag, files):
    h = hashlib.sha256(("%s-v%d\n" % (tag, VERSION)).encode())
    for f in sorted(files, key=lambda f: f.rel.lower()):
        h.update(("%s\t%d\t%s\n" % (f.rel, f.size, f.digest)).encode("utf-8"))
    return h.hexdigest()


class Entry:
    """One staged unit: a folder whose files mirror `files` byte for byte (scope: the part of it this entry owns)."""

    def __init__(self, kind, spec, files, scope, source):
        self.kind, self.spec, self.files, self.scope, self.source = kind, spec, files, scope, source
        self.name = self.dest = self.key = None
        self.need = []      # files to copy (missing, or different bytes)
        self.extra = []     # files in scope the source does not have (removed)
        self.verified = 0   # staged files hashed by this pass

    def place(self, parent, exe_sha=None):
        self.key = content_key(self.kind, self.files)
        self.name = ("exe-%s-%s" % (exe_sha[:12], self.key[:8])) if self.kind == "exe" else (
            "%s-%s" % (self.kind, self.key[:16]))
        self.dest = os.path.join(parent, self.name)

    @property
    def bytes(self):
        return sum(f.size for f in self.files)

    def measure(self, verify):
        """verify: 'full' re-hashes every staged file; 'quick' trusts one whose size and mtime match the manifest
        that recorded its digest; 'none' (a plan) checks presence and size only."""
        man = read_json(os.path.join(self.dest, MANIFEST)) or {}
        recorded = man.get("files", {}) if man.get("key") == self.key else {}
        todo = []
        self.need = []
        for f in self.files:
            p = os.path.join(self.dest, *f.rel.split("/"))
            try:
                st = os.stat(p, follow_symlinks=False)
            except OSError:
                self.need.append(f)
                continue
            if st.st_size != f.size:
                self.need.append(f)
                continue
            r = recorded.get(f.rel)
            if verify == "none" or (verify == "quick" and r and r[0] == f.size and r[1] == f.digest and
                                    r[2] == st.st_mtime_ns):
                continue
            todo.append((f, p))
        self.verified = len(todo)
        if todo:
            with concurrent.futures.ThreadPoolExecutor(4) as pool:     # NVMe: parallel reads help
                for (f, _p), d in zip(todo, pool.map(lambda t: sha256_file(t[1]), todo)):
                    if d != f.digest:
                        self.need.append(f)
        want = {f.rel.lower() for f in self.files}
        self.extra = []
        top = os.path.join(self.dest, *self.scope.split("/")) if self.scope else self.dest
        if os.path.isdir(top):
            for dirpath, dirnames, filenames in os.walk(top):
                dirnames[:] = [x for x in dirnames if not is_link(os.path.join(dirpath, x))]
                for n in filenames:
                    rel = os.path.relpath(os.path.join(dirpath, n), self.dest).replace("\\", "/")
                    if rel != MANIFEST and rel.lower() not in want:
                        self.extra.append(rel)
        return self


def build(stage, exe, root, source, env):
    """-> (entries [content, exe, shader?], info) with file lists; names come later (place) once digests are known."""
    exe = os.path.abspath(exe)
    if not os.path.isfile(exe) or not exe.lower().endswith(".exe"):
        raise Refused("--exe %s is not an existing .exe" % exe)
    root = os.path.abspath(root)
    refusals = gate_refusals(env)
    if refusals:
        raise Refused("; ".join(refusals) + " - roll without --stage")
    src = os.path.abspath(source) if source else content_root_of(exe, root)
    if not src or not os.path.isdir(os.path.join(src, DISC)):
        raise Refused("no '%s' %s" % (DISC, ("in --source %s" % src) if source else
                      "beside the exe, in its parent or in the working directory %s (pass --source <the folder "
                      "holding it>)" % root))
    if under(exe, stage) or under(src, stage):
        raise Refused("the source %s is inside the stage %s: pass the D: source" % (exe, stage))
    ssrc, gs, swhy = shader_source(root, exe, env)
    if ssrc and not os.path.isdir(ssrc):
        raise Refused("SR_SHADER_CACHE_DIR %s is not a folder" % ssrc)
    content = Entry("content", None, disc_files(src), DISC, os.path.join(src, DISC))
    exe_list, notes = exe_folder_files(exe, src, root)
    exe_e = Entry("exe", None, exe_list, "", os.path.dirname(exe))
    entries = [content, exe_e]
    nbsrc, nbwhy = nb_source(root, exe, env)
    if nbsrc and not os.path.isdir(nbsrc):
        raise Refused("SR_NB_AOT_DIR %s is not a folder" % nbsrc)
    if ssrc or nbsrc:
        sf = flat_files(ssrc, "dxil/") if ssrc else []
        if gs and os.path.isfile(gs):
            sf.append(one_file("point_sprite.gs.dxil", gs))
        if nbsrc:
            blobs = flat_files(nbsrc, "nb/")
            if not any(f.rel.lower().endswith(".dxbc") and f.size > 32 for f in blobs):
                raise Refused("native blob directory has no nonempty DXBC candidates: %s" % nbsrc)
            sf += blobs
        entries.append(Entry("shader", None, sf, "", ssrc or nbsrc))
        notes.append("shader cache from %s (%s)%s" % (ssrc, swhy, "" if gs and os.path.isfile(gs) else
                                                     "; no point_sprite.gs.dxil where the game looks"))
        notes.append("resolve kernels (data/shader_cache/nb): %s" % (("from %s (%s)" % (nbsrc, nbwhy)) if nbsrc else nbwhy))
    else:
        notes.append("shader cache: " + swhy)
    for k in ("SR_PORT_FEATURES_DIR", "SR_GLYPHS_DIR", "SR_DXC_DIR", "SR_SHADER_RUNTIME_DIR"):
        if env.get(k):
            notes.append("%s=%s is read where it is (an explicit gate, not staged)" % (k, env[k]))
    if volume(stage) == volume(src) or volume(stage) == volume(exe):
        notes.append("the stage %s is on the same volume as the source: no I/O isolation" % stage)
    rels = {f.rel.lower() for f in exe_list}
    scopes = {"exe": True, "disc": True, "shader": bool(ssrc), "nb": bool(nbsrc),
              "dxc": "dxcompiler.dll" in rels or "dxil.dll" in rels,
              "port_features": any(r.startswith("port_features/") for r in rels),
              "fonts": any(r.startswith("data/fonts/") for r in rels),
              "glyphs": any(r.startswith("data/glyphs/") for r in rels)}
    dev = devtools_record(env)
    if dev:
        notes.append("DEV-TOOL-ASSISTED roll (the preset warp): " + " ".join("%s=%s" % kv for kv in dev.items()))
    info = {"exe_src": exe, "content_src": src, "shader_src": ssrc, "shader_why": swhy, "gs_src": gs,
            "nb_src": nbsrc, "nb_why": nbwhy, "root": root, "notes": notes, "stage": stage, "scopes": scopes,
            "dev_tools": dev}
    return entries, info


def place_all(entries, info, stage):
    content, exe_e = entries[0], entries[1]
    content.place(stage)
    exe_digest = next(f.digest for f in exe_e.files if f.rel == os.path.basename(info["exe_src"]))
    exe_e.place(content.dest, exe_digest)
    for e in entries[2:]:
        e.place(stage)
    info["exe_sha256"] = exe_digest
    info["staged_exe"] = os.path.join(exe_e.dest, os.path.basename(info["exe_src"]))
    info["content_root"] = content.dest
    info["shader_cache_dir"] = os.path.join(entries[2].dest, "dxil") if info.get("shader_src") else None
    info["nb_dir"] = os.path.join(entries[2].dest, "nb") if len(entries) > 2 and info.get("nb_src") else None


# ------------------------------------------------------------------------------------------------ the stage root

def check_stage_path(stage):
    """An absolute folder of its own, not a drive root, with no junction / symlink in it or any ancestor."""
    stage = os.path.abspath(stage)
    if not os.path.splitdrive(stage)[1].strip("\\/"):
        raise Refused("--stage %s is a drive root: name a folder of its own (e.g. J:\\sr_stage)" % stage)
    p = stage
    while True:
        if os.path.lexists(p) and is_link(p):
            raise Refused("%s is a junction / symlink: the stage must be a real folder on a real path (%s)"
                          % (p, stage))
        parent = os.path.dirname(p)
        if parent == p:
            break
        p = parent
    return stage


def claim_root(stage, create):
    """Ours (marker), empty, or absent. Creates only the stage folder itself (its parent must exist). A folder holding
    nothing but stage.lock is an empty stage (a clean that met a starting stage_roll leaves exactly that)."""
    if os.path.isdir(stage):
        marker = os.path.join(stage, MARKER)
        if os.path.lexists(marker):
            if is_link(marker) or not os.path.isfile(marker):
                raise Refused("%s is not a regular file" % marker)
            return
        if set(os.listdir(stage)) - {LOCK}:
            raise Refused("%s is not empty and has no %s: not a stage this tool made - refusing to write there"
                          % (stage, MARKER))
        if create:
            write_marker(stage)
        return
    if os.path.lexists(stage):
        raise Refused("%s exists and is not a folder" % stage)
    parent = os.path.dirname(stage)
    if not os.path.isdir(parent):
        raise Refused("the parent %s does not exist (stage_roll creates only the stage folder itself)" % parent)
    if create:
        os.mkdir(stage)
        write_marker(stage)


def write_marker(stage):
    with os.fdopen(create_excl(os.path.join(stage, MARKER), stage), "w", encoding="utf-8") as f:
        f.write("SR roll stage (tools/perf/stage_roll.py), created %s by pid %d.\n"
                "Everything here is a copy of files on D: (the game's read set for live rolls) plus the game's own\n"
                "generated mod cache. Remove it with: python tools/perf/stage_roll.py clean --stage %s\n"
                % (now(), os.getpid(), stage))


def require_ours(stage):
    if not os.path.isdir(stage) or not os.path.isfile(os.path.join(stage, MARKER)):
        raise Refused("%s has no %s: not a stage this tool made - nothing to do there" % (stage, MARKER))


def assert_writable_entry(path, stage):
    """Nothing is written through a link: the entry (if present) holds none and resolves inside the stage."""
    if os.path.lexists(path):
        bad = links_in(path)
        if bad:
            raise Refused("a junction / symlink inside the stage entry %s: %s - nothing is written through it"
                          % (path, ", ".join(bad[:5])))
        if not under_resolved(path, stage):
            raise Refused("%s resolves outside the stage %s" % (path, stage))


def _lock_byte(fd, on):
    os.lseek(fd, 0, os.SEEK_SET)
    if os.name == "nt":
        import msvcrt
        msvcrt.locking(fd, msvcrt.LK_NBLCK if on else msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.flock(fd, (fcntl.LOCK_EX | fcntl.LOCK_NB) if on else fcntl.LOCK_UN)


class Lock:
    """<stage>/stage.lock: an OS byte-range lock on an open handle (msvcrt.locking). The OS drops it when the holder's
    handle closes or the holder dies, so there is no stale lock and no recovery logic. The lock file is opened without
    truncation, refused when it is a link or a folder, and checked to be the file at that name once open."""

    def __init__(self, stage, wait_s):
        self.path = os.path.join(stage, LOCK)
        self.fd = None
        if os.path.lexists(self.path):
            st = os.stat(self.path, follow_symlinks=False)
            if is_link_stat(st) or not stat.S_ISREG(st.st_mode):
                raise Refused("%s is not a regular file (a junction / symlink / folder - an old-format lock folder "
                              "is removed by hand when no stage_roll runs)" % self.path)
        check_parent(self.path, stage)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
        try:
            a, b = os.fstat(fd), os.stat(self.path, follow_symlinks=False)
            if is_link_stat(b) or (a.st_ino, a.st_dev) != (b.st_ino, b.st_dev):
                raise Refused("%s changed while it was opened - refusing" % self.path)
            deadline = time.time() + wait_s
            while True:
                try:
                    _lock_byte(fd, True)
                    self.fd, fd = fd, None
                    return
                except OSError:
                    if time.time() >= deadline:
                        raise Refused("another stage_roll holds %s (the OS lock; it ends when that process does)"
                                      % self.path)
                    time.sleep(0.5)
        finally:
            if fd is not None:
                os.close(fd)

    def release(self):
        if self.fd is not None:
            try:
                _lock_byte(self.fd, False)
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None


# ------------------------------------------------------------------------------------------------ leases

def lease_path(stage, lease_id):
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", lease_id or ""):
        raise Refused("lease id %r: use [A-Za-z0-9_.-], up to 80 characters" % lease_id)
    return os.path.join(stage, LEASES, lease_id + ".json")


def holders(doc):
    """[(pid, creation time or None)] of a lease (a round-1 lease has one holder_pid)."""
    if isinstance(doc.get("holders"), list):
        return [(int(h.get("pid") or 0), h.get("created")) for h in doc["holders"] if isinstance(h, dict)]
    return [(int(doc.get("holder_pid") or 0), doc.get("holder_created"))]


def holder_alive(pid, created):
    return pid_alive(pid) and (created is None or proc_created(pid) in (None, created))


def lease_live(doc, t=None):
    """Live while ANY holder runs (pid + creation time: a reused pid is not the holder) and within its ttl."""
    t = time.time() if t is None else t
    if not isinstance(doc, dict) or t - float(doc.get("created", 0)) > float(doc.get("ttl_s", 0)):
        return False
    return any(holder_alive(p, c) for p, c in holders(doc))


def leases(stage, prune=False):
    """The live leases [(id, doc)]; prune (under the lock) removes the stale ones."""
    d = os.path.join(stage, LEASES)
    out = []
    if not os.path.isdir(d) or is_link(d):
        return out
    for n in sorted(os.listdir(d)):
        p = os.path.join(d, n)
        if not n.endswith(".json") or is_link(p):
            continue
        doc = read_json(p)
        if lease_live(doc):
            out.append((n[:-5], doc))
        elif prune:
            try:
                unlink_file(p, stage)
            except (OSError, RuntimeError):
                pass
    return out


def leased_by(path, live):
    """The lease ids holding path: one lists it, or something inside it."""
    return [i for i, doc in live for lp in doc.get("entries", []) if under(lp, path)]


def write_lease(stage, lease_id, pid, entries, ttl_h, note):
    p = lease_path(stage, lease_id)
    d = os.path.dirname(p)
    if not os.path.lexists(d):
        check_parent(d, stage)
        os.mkdir(d)
    write_json(p, {"id": lease_id, "holders": [{"pid": pid, "created": proc_created(pid)}], "created": time.time(),
                   "ttl_s": ttl_h * 3600.0, "entries": entries, "note": note, "when": now()}, stage)
    return p


# ------------------------------------------------------------------------------------------------ retirement

def unlink_file(p, root):
    """Remove one regular file inside root: refused when it is a reparse point or resolves outside, checked just
    before the unlink (the residual race is the microseconds between that check and the unlink)."""
    st = os.stat(p, follow_symlinks=False)
    if is_link_stat(st) or not stat.S_ISREG(st.st_mode):
        raise RuntimeError("%s is not a regular file (a junction / symlink appeared?) - deletion stopped" % p)
    if not under(os.path.realpath(p), os.path.realpath(root)):
        raise RuntimeError("%s resolves outside %s - deletion stopped" % (p, root))
    os.unlink(p)


def remove_tree(path, root):
    """Delete the folder path (strictly inside root) file by file: os.scandir without following links, a reparse
    point at any level stops it, each file's resolved path is re-checked inside root just before its unlink, and only
    emptied folders are removed (os.rmdir) - never a recursive delete through a path."""
    real_root = os.path.realpath(root)
    if not under(path, root) or norm(path) == norm(root):
        raise RuntimeError("refusing to delete %s: not inside %s" % (path, root))

    def inside(p):
        rp = os.path.realpath(p)
        return under(rp, real_root) and norm(rp) != norm(real_root)

    def walk(d):
        st = os.stat(d, follow_symlinks=False)
        if is_link_stat(st) or not stat.S_ISDIR(st.st_mode) or not inside(d):
            raise RuntimeError("%s is a junction / symlink or resolves outside the stage - deletion stopped" % d)
        with os.scandir(d) as it:
            items = list(it)
        for e in items:
            est = e.stat(follow_symlinks=False)
            if is_link_stat(est):
                raise RuntimeError("%s is a junction / symlink - deletion stopped" % e.path)
            if stat.S_ISDIR(est.st_mode):
                walk(e.path)
            else:
                unlink_file(e.path, root)
        if is_link(d) or not inside(d):
            raise RuntimeError("%s changed into a junction / symlink - deletion stopped" % d)
        os.rmdir(d)          # an empty folder only

    walk(path)


def deletable(path, stage):
    """'' when path may be deleted: inside the stage once resolved, and no junction / symlink at or under it."""
    bad = links_in(path)
    if bad:
        return "holds a junction / symlink (%s)" % ", ".join(bad[:3])
    if not under_resolved(path, stage) or norm(os.path.realpath(path)) == norm(os.path.realpath(stage)):
        return "resolves outside the stage"
    return ""


def retire(path, root):
    """Remove an entry unless something in it is in use: it is renamed first (Windows refuses to rename a folder
    holding a running exe), then deleted. Links and paths resolving outside are refused before any change."""
    why = deletable(path, root)
    if why:
        return False, why
    if not os.path.isdir(path):
        try:
            unlink_file(path, root)
        except (OSError, RuntimeError) as e:
            return False, str(e)
        return True, ""
    trash = "%s.trash-%d-%d" % (path, os.getpid(), int(time.time() * 1000) % 100000)
    try:
        os.rename(path, trash)
    except OSError as e:
        return False, "in use or locked (%s)" % (e.strerror or e)
    try:
        remove_tree(trash, root)
    except (OSError, RuntimeError) as e:
        return False, "renamed to %s but not fully removed (%s)" % (trash, e)
    return True, ""


def entries_on_disk(stage):
    """[(kind, name, path, manifest-or-None)] of every entry in the stage, the exe folders inside content roots too."""
    out = []
    if not os.path.isdir(stage):
        return out
    for n in sorted(os.listdir(stage)):
        p = os.path.join(stage, n)
        kind = n.split("-", 1)[0]
        if kind not in ("content", "shader") or ".trash-" in n or not os.path.isdir(p) or is_link(p):
            continue
        out.append((kind, n, p, read_json(os.path.join(p, MANIFEST))))
        if kind == "content":
            for m in sorted(os.listdir(p)):
                q = os.path.join(p, m)
                if m.startswith("exe-") and ".trash-" not in m and os.path.isdir(q) and not is_link(q):
                    out.append(("exe", m, q, read_json(os.path.join(q, MANIFEST))))
    return out


def tree_bytes(path):
    if is_link(path):
        return 0
    if not os.path.isdir(path):
        return os.path.getsize(path) if os.path.exists(path) else 0
    total = 0
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if not is_link(os.path.join(dirpath, d))]
        for n in filenames:
            try:
                total += os.stat(os.path.join(dirpath, n), follow_symlinks=False).st_size
            except OSError:
                pass
    return total


def retire_plan(stage, current, other_exes, other_shaders, live, other_contents=True):
    """Entries to retire, never one of `current` nor a leased one: exe folders (of a current content root) beyond the
    other_exes most recently used, shader caches beyond other_shaders, and (other_contents) every content root but
    the current. other_exes / other_shaders < 0: keep all of that kind."""
    cur = {norm(p) for p in current}
    exes, shaders, contents = [], [], []
    for kind, _name, path, man in entries_on_disk(stage):
        if norm(path) in cur or leased_by(path, live):
            continue
        t = (man or {}).get("last_used", "")
        if kind == "content":
            contents.append(path)
        elif kind == "shader":
            shaders.append((t, path))
        elif norm(os.path.dirname(path)) in cur:     # the exe folders of a retired content root go with it
            exes.append((t, path))
    out = list(contents) if other_contents else []
    if other_exes >= 0:
        out += [p for _t, p in sorted(exes, reverse=True)[other_exes:]]
    if other_shaders >= 0:
        out += [p for _t, p in sorted(shaders, reverse=True)[other_shaders:]]
    return out


# ------------------------------------------------------------------------------------------------ plan / stage

def summary_lines(entries, info, free, need_bytes, floor):
    lines = ["stage %s: exe %s (sha256 %s), disc from %s, shader cache %s" % (
        info["stage"], info["exe_src"], (info.get("exe_sha256") or "pending")[:12], info["content_src"],
        info["shader_src"] or ("- " + info["shader_why"]))]
    for e in entries:
        lines.append("  %-7s %-34s %5d files %10s; to copy %d files %s%s%s" % (
            e.kind, e.name or "%s-(key pending: sources not hashed yet)" % e.kind, len(e.files), human(e.bytes),
            len(e.need), human(sum(f.size for f in e.need)),
            ("; %d stale files to remove" % len(e.extra)) if e.extra else "",
            ("; %d staged files verified" % e.verified) if e.verified else ""))
    lines.append("  staged set %s; to copy %s; %s free %s -> %s after (reserve %s for the game's mod cache; "
                 "floor %s)" % (human(sum(e.bytes for e in entries)), human(need_bytes), volume(info["stage"]),
                                gib(free), gib(free - need_bytes - RESERVE), gib(RESERVE), gib(floor)))
    for n in info["notes"]:
        lines.append("  note: " + n)
    return lines


def floor_refusal(stage, free, need_bytes, floor, when):
    return Refused("%s: %s has %s free; %s (+%s reserve) would leave %s, below the %s floor (owner 2026-10-14: be "
                   "wary about space). Free space first: python tools/perf/stage_roll.py clean --stage %s" % (
                       when, volume(stage), gib(free), ("copying %s" % human(need_bytes)) if need_bytes else
                       "the game's own writes", gib(RESERVE), gib(free - need_bytes - RESERVE), gib(floor), stage))


def prepare(a, env):
    stage = check_stage_path(a.stage)
    claim_root(stage, create=False)
    entries, info = build(stage, a.exe, a.root or os.getcwd(), a.source, env)
    total = sum(e.bytes for e in entries)
    if total > a.max_gib * GIB:
        raise Refused("the read set is %s, over the %.0f GiB limit (--max-gib): check --exe / --source" % (
            human(total), a.max_gib))
    return stage, entries, info


def cmd_plan(a, env):
    stage, entries, info = prepare(a, env)
    dg = Digests(stage)
    unknown = sum(dg.fill(e.files, hash_missing=False)[2] for e in entries)
    floor = int(a.floor_gib * GIB)
    if unknown:
        for e in entries:
            e.need = list(e.files)   # an upper bound: a key needs every digest
    else:
        place_all(entries, info, stage)
        for e in entries:
            e.measure("none")
    live = leases(stage)
    retire_list = [] if unknown else retire_plan(stage, [e.dest for e in entries],
                                                 a.keep_exes - 1 if a.keep_exes > 0 else -1, KEEP_SHADERS - 1, live)
    freed = sum(tree_bytes(p) for p in retire_list)
    need_bytes = sum(f.size for e in entries for f in e.need)
    free = free_bytes(stage) + freed
    for ln in summary_lines(entries, info, free, need_bytes, floor):
        print(ln)
    if unknown:
        print("  %d source files are not hashed yet (a stage reads them once to name the entries)" % unknown)
    for p in retire_list:
        print("  would retire %s (%s)" % (p, human(tree_bytes(p))))
    if a.files:
        for e in entries:
            for f in e.files:
                print("    %-7s %12d  %s" % (e.kind, f.size, f.rel))
    if free - need_bytes - RESERVE < floor:
        print("REFUSED (a stage would be): %s would drop below the %s floor" % (volume(stage), gib(floor)))
        return 2
    print("PLAN OK (nothing written)")
    return 0


def make_dirs(path, root):
    """Create path's missing folders inside root, one at a time, never through a link."""
    missing = []
    p = os.path.abspath(path)
    while not os.path.lexists(p):
        missing.append(p)
        p = os.path.dirname(p)
    for d in reversed(missing):
        check_parent(d, root)
        os.mkdir(d)
    if is_link(path) or not os.path.isdir(path) or not under_resolved(path, root):
        raise Refused("%s is a junction / symlink or resolves outside the stage - nothing is written there" % path)


def copy_one(f, dest, root):
    """Copy one file through <file>.part created with O_EXCL, hashing the bytes as they are written: refused unless
    they are the digest's bytes; the rename replaces the name, never writes through it."""
    dst = os.path.join(dest, *f.rel.split("/"))
    make_dirs(os.path.dirname(dst), root)
    tmp = dst + ".part"
    h = hashlib.sha256()
    with open(f.src, "rb") as fi, os.fdopen(create_excl(tmp, root), "wb") as fo:
        while True:
            b = fi.read(CHUNK)
            if not b:
                break
            h.update(b)
            fo.write(b)
    st = os.stat(f.src)
    if st.st_size != f.size or st.st_mtime_ns != f.mtime or h.hexdigest() != f.digest:
        unlink_file(tmp, root)
        raise RuntimeError("%s changed while it was being staged (its bytes are not the digest's)" % f.src)
    os.utime(tmp, ns=(f.mtime, f.mtime))
    os.replace(tmp, dst)


def cmd_stage(a, env):
    t0 = time.time()
    stage, entries, info = prepare(a, env)
    floor = int(a.floor_gib * GIB)
    keep_other_exes = a.keep_exes - 1 if a.keep_exes > 0 else -1
    if a.lease_id:
        lease_path(stage, a.lease_id)      # a bad id is refused before anything is created
    # before anything is created: a stage that cannot fit even after its retirements touches nothing
    dg = Digests(stage)
    if not sum(dg.fill(e.files, hash_missing=False)[2] for e in entries):
        place_all(entries, info, stage)
        for e in entries:
            e.measure("none")
        pre_need = sum(f.size for e in entries for f in e.need)
        pre_free = free_bytes(stage) + sum(tree_bytes(p) for p in retire_plan(
            stage, [e.dest for e in entries], keep_other_exes, KEEP_SHADERS - 1, leases(stage)))
    else:
        pre_need, pre_free = sum(e.bytes for e in entries), free_bytes(stage)
    if pre_free - pre_need - RESERVE < floor:
        raise floor_refusal(stage, pre_free, pre_need, floor, "before staging")
    lock = None
    for attempt in (0, 1, 2):       # a clean may be removing an emptied stage folder right now
        claim_root(stage, create=True)
        try:
            lock = Lock(stage, a.lock_wait)
            break
        except FileNotFoundError:
            if attempt == 2:
                raise
    try:
        claim_root(stage, create=True)          # again under the lock: a clean may have retired the root meanwhile
        bad = [p for p in links_in(stage) if norm(p) != norm(stage)]
        if bad:                                 # before ANY write: nothing goes through a link
            raise Refused("a junction / symlink inside the stage (%s): nothing written - remove the link by hand"
                          % ", ".join(bad[:5]))
        dg = Digests(stage)
        # the retail disc is read-only: its cached digests stand; the exe folder and the shader cache are mutable and
        # re-hashed under --verify full (sr_drive); --verify quick (the pre-stage sr_drive follows) may use the cache
        hashed = [dg.fill(e.files, hash_missing=True, use_cache=e.kind == "content" or a.verify == "quick")
                  for e in entries]
        dg.save()
        place_all(entries, info, stage)
        live = leases(stage, prune=True)
        retired, kept = [], []
        for p in retire_plan(stage, [e.dest for e in entries], keep_other_exes, KEEP_SHADERS - 1, live):
            ok, why = retire(p, stage)
            (retired if ok else kept).append(p if ok else "%s: %s" % (p, why))
        for e in entries:
            assert_writable_entry(e.dest, stage)
            e.measure(a.verify)
        need_bytes = sum(f.size for e in entries for f in e.need)
        free = free_bytes(stage)
        if sum(h[0] for h in hashed):
            print("  hashed %d source files (%s) on D: (the disc's digests are cached in %s)"
                  % (sum(h[0] for h in hashed), human(sum(h[1] for h in hashed)), DIGESTS), flush=True)
        for ln in summary_lines(entries, info, free, need_bytes, floor):
            print(ln, flush=True)
        if free - need_bytes - RESERVE < floor:
            raise floor_refusal(stage, free, need_bytes, floor, "before copying")
        left, mark, step = need_bytes, 512 * 1024 ** 2, 512 * 1024 ** 2
        for e in entries:
            make_dirs(e.dest, stage)
            assert_writable_entry(e.dest, stage)
            for rel in e.extra:
                unlink_file(os.path.join(e.dest, *rel.split("/")), stage)
            for f in e.need:
                fr = free_bytes(stage)
                if fr - left - RESERVE < floor:      # another writer on the volume took the headroom
                    raise floor_refusal(stage, fr, left, floor, "while copying")
                copy_one(f, e.dest, stage)
                left -= f.size
                if need_bytes - left >= mark:
                    print("stage: copied %s of %s" % (human(need_bytes - left), human(need_bytes)), flush=True)
                    mark += step
        for e in entries:
            copied = {f.rel for f in e.need}
            # every file is accounted for: copied ones were hashed as they were written, the others verified above
            e.measure("none")
            if e.need or e.extra:
                raise RuntimeError("%s still differs from its source after staging (%d to copy, %d stale)"
                                   % (e.name, len(e.need), len(e.extra)))
            man_path = os.path.join(e.dest, MANIFEST)
            man = read_json(man_path) or {}
            files = {}
            for f in e.files:
                files[f.rel] = [f.size, f.digest, os.stat(os.path.join(e.dest, *f.rel.split("/"))).st_mtime_ns]
            doc = {"version": VERSION, "kind": e.kind, "name": e.name, "key": e.key, "source": e.source,
                   "bytes": e.bytes, "files": files,
                   "created": (man.get("created") if man.get("key") == e.key else None) or now(), "last_used": now(),
                   "copied_this_time": len(copied)}
            write_json(man_path, doc, stage)
        free_after = free_bytes(stage)
        if free_after - RESERVE < floor:
            raise floor_refusal(stage, free_after, 0, floor, "after staging, before the launch")
        lease_file = None
        if a.lease_id:
            lease_file = write_lease(stage, a.lease_id, a.lease_pid or os.getpid(), [e.dest for e in entries],
                                     a.lease_ttl_h, a.lease_note)
        seconds = round(time.time() - t0, 1)
        staged_bytes = sum(e.bytes for e in entries)
        lines = ["%s %s (%d files, %s)" % (e.kind, e.dest, len(e.files), human(e.bytes)) for e in entries]
        lines.append("copied %s of %s staged in %.1f s; %s free %s (was %s; floor %s); verify %s" % (
            human(need_bytes), human(staged_bytes), seconds, volume(stage), gib(free_after), gib(free), gib(floor),
            a.verify))
        if lease_file:
            lines.append("lease %s held for pid %d (ttl %.0f h; lease-add adds the game)" % (
                a.lease_id, a.lease_pid or os.getpid(), a.lease_ttl_h))
        lines += ["retired " + p for p in retired] + ["retire skipped: " + p for p in kept]
        lines += ["note: " + n for n in info["notes"]]
        env_out = {}
        if info["shader_cache_dir"]:
            env_out["SR_SHADER_CACHE_DIR"] = info["shader_cache_dir"]
        if info["nb_dir"]:
            env_out["SR_NB_AOT_DIR"] = info["nb_dir"]
        doc = {"version": VERSION, "stage": stage, "exe": info["staged_exe"], "exe_sha256": info["exe_sha256"],
               "content_root": info["content_root"], "shader_cache_dir": info["shader_cache_dir"],
               "nb_dir": info["nb_dir"], "env": env_out,
               "source": {"exe": info["exe_src"], "content_root": info["content_src"],
                          "shader_cache_dir": info["shader_src"], "point_sprite_gs": info["gs_src"],
                          "nb_dir": info["nb_src"], "root": info["root"]},
               "scopes": info["scopes"],
               "entries": [{"kind": e.kind, "name": e.name, "path": e.dest, "files": len(e.files), "bytes": e.bytes}
                           for e in entries],
               "copied_bytes": need_bytes, "staged_bytes": staged_bytes, "free_before": free,
               "free_after": free_after, "floor": floor, "reserve": RESERVE, "verify": a.verify,
               "lease": {"id": a.lease_id, "file": lease_file} if lease_file else None,
               "retired": retired, "retire_skipped": kept, "notes": info["notes"], "seconds": seconds,
               "dev_tools": info["dev_tools"],
               # Staging establishes dev-tool taint only. Port features, forcing,
               # saves and actual runtime evidence determine a natural run.
               "run_class": "dev-tool-assisted" if info["dev_tools"] else "unclassified",
               "when": now(), "lines": lines}
        if a.json:
            write_json(os.path.abspath(a.json), doc)
        for ln in lines:
            print("stage: " + ln)
        print("STAGE_OK exe=%s" % info["staged_exe"], flush=True)
        return 0
    finally:
        lock.release()


def cmd_lease_add(a):
    """Add a holder (sr_drive: the game it just launched) to a lease: it lives while any holder runs. No stage lock:
    the lease file belongs to its driver and is replaced atomically."""
    stage = check_stage_path(a.stage)
    require_ours(stage)
    p = lease_path(stage, a.lease_id)
    doc = read_json(p) if os.path.isfile(p) and not is_link(p) else None
    if not doc:
        raise Refused("lease %s: none to add a holder to" % a.lease_id)
    if not pid_alive(a.pid):
        raise Refused("pid %d is not running" % a.pid)
    hs = [{"pid": q, "created": c} for q, c in holders(doc) if q != a.pid]
    hs.append({"pid": a.pid, "created": proc_created(a.pid)})
    doc["holders"] = hs
    doc.pop("holder_pid", None)
    doc.pop("holder_created", None)
    write_json(p, doc, stage)
    print("lease %s: holders %s" % (a.lease_id, ", ".join(str(h["pid"]) for h in hs)))
    return 0


def cmd_release(a):
    stage = check_stage_path(a.stage)
    p = lease_path(stage, a.lease_id)
    if not os.path.lexists(p):
        print("lease %s: none (already released or never taken)" % a.lease_id)
        return 0
    if a.handoff_pid:
        doc = read_json(p) or {}
        if not pid_alive(a.handoff_pid):
            unlink_file(p, stage)
            print("lease %s: pid %d is not running - released" % (a.lease_id, a.handoff_pid))
            return 0
        doc["holders"] = [{"pid": a.handoff_pid, "created": proc_created(a.handoff_pid)}]
        doc.pop("holder_pid", None)
        doc.pop("holder_created", None)
        doc["note"] = (doc.get("note") or "") + " | handed to pid %d" % a.handoff_pid
        write_json(p, doc, stage)
        print("lease %s: handed to pid %d" % (a.lease_id, a.handoff_pid))
        return 0
    unlink_file(p, stage)
    print("lease %s: released" % a.lease_id)
    return 0


# ------------------------------------------------------------------------------------------------ status / clean

def cmd_status(a):
    stage = check_stage_path(a.stage)
    if not os.path.isdir(stage):
        print("%s: no stage; %s free %s" % (stage, volume(stage), gib(free_bytes(stage))))
        return 0
    require_ours(stage)
    live = leases(stage)
    total = 0
    for kind, name, path, man in entries_on_disk(stage):
        b = tree_bytes(path) if kind != "content" else sum(
            tree_bytes(os.path.join(path, x)) for x in os.listdir(path) if not x.startswith("exe-"))
        total += b
        held = leased_by(path, live)
        print("%-7s %-40s %10s  last used %s%s%s" % (kind, name, human(b), (man or {}).get("last_used", "-"),
                                                    "" if man else "  (incomplete: no %s)" % MANIFEST,
                                                    ("  LEASED by " + ", ".join(sorted(set(held)))) if held else ""))
    for i, doc in live:
        print("lease   %-40s pids %s since %s (%s)" % (i, ", ".join(str(q) for q, _c in holders(doc)), doc.get("when"),
                                                      doc.get("note", "")))
    print("total %s in %s; %s free %s" % (human(total), stage, volume(stage), gib(free_bytes(stage))))
    return 0


def cmd_clean(a):
    stage = check_stage_path(a.stage)
    if not os.path.isdir(stage):
        print("%s: nothing to clean" % stage)
        return 0
    if set(os.listdir(stage)) == {LOCK} and not a.dry_run:     # an emptied stage a starting stage_roll left behind
        Lock(stage, a.lock_wait).release()
        try:
            unlink_file(os.path.join(stage, LOCK), stage)
            os.rmdir(stage)
            print("removed the empty stage folder %s" % stage)
        except OSError:
            print("left the stage folder %s (another stage_roll is starting in it)" % stage)
        return 0
    require_ours(stage)
    # every check before any change: a junction / symlink anywhere in the stage refuses the whole clean
    bad = [p for p in links_in(stage) if norm(p) != norm(stage)]
    if bad:
        raise Refused("a junction / symlink inside the stage (%s): nothing removed - remove the link by hand"
                      % ", ".join(bad[:5]))
    lock = None if a.dry_run else Lock(stage, a.lock_wait)
    try:
        live = leases(stage, prune=not a.dry_run)
        if a.keep_exes is not None:
            contents = [p for k, _n, p, _m in entries_on_disk(stage) if k == "content"]
            victims = retire_plan(stage, contents, max(a.keep_exes, 0), -1, live, other_contents=False)
        else:
            victims = [os.path.join(stage, n) for n in sorted(os.listdir(stage)) if n not in (MARKER, LOCK, LEASES)]
        for p in victims:
            why = deletable(p, stage)
            if why:
                raise Refused("%s %s: nothing removed" % (p, why))
        freed, failed = 0, []
        for p in victims:
            held = leased_by(p, live)
            if held:
                failed.append(p)
                print("KEPT %s: leased by %s" % (p, ", ".join(sorted(set(held)))))
                if a.keep_exes is None and os.path.isdir(p) and os.path.basename(p).startswith("content-"):
                    for m in sorted(os.listdir(p)):      # trim the unleased exe folders inside a kept content root
                        q = os.path.join(p, m)
                        if m.startswith("exe-") and not leased_by(q, live) and not a.dry_run:
                            ok, why = retire(q, stage)
                            print(("removed %s" if ok else "KEPT %s: " + why) % q)
                continue
            b = tree_bytes(p)
            if a.dry_run:
                print("would remove %s (%s)" % (p, human(b)))
                continue
            ok, why = retire(p, stage)
            if ok:
                freed += b
                print("removed %s (%s)" % (p, human(b)))
            else:
                failed.append(p)
                print("KEPT %s: %s" % (p, why))
        if a.dry_run:
            return 0
        if a.keep_exes is None and not failed and not live:
            # the stage is retired WHILE the lock is held: the leases folder, then the marker; a stage_roll that
            # takes the lock next finds a folder holding only stage.lock - an empty stage it may claim again
            ld = os.path.join(stage, LEASES)
            if os.path.isdir(ld) and not is_link(ld) and not os.listdir(ld):
                os.rmdir(ld)
            unlink_file(os.path.join(stage, MARKER), stage)
            lock.release()
            lock = None
            try:                                 # fails harmlessly when another stage_roll has opened the lock
                unlink_file(os.path.join(stage, LOCK), stage)
                os.rmdir(stage)
                print("removed the stage folder %s" % stage)
            except OSError:
                print("left the stage folder %s (another stage_roll is starting in it)" % stage)
        print("freed %s; %s free %s" % (human(freed), volume(stage), gib(free_bytes(stage))))
        return 1 if failed else 0
    finally:
        if lock:
            lock.release()


# ------------------------------------------------------------------------------------------------ check

SHADER_LINE = re.compile(r"\[SHADER_CACHE\] loaded \d+ VS \+ \d+ PS translated shaders from (.+?) \(")
MOD_LINE = re.compile(r'\[MOD\] overlay adds .*? -> "([^"]+)"')
# a SUCCESSFUL open ("-> handle=N"; "dir path=" for a folder) is the witness; FILE_NOT_FOUND, "open failed" and the
# alias lines are not (an alias's target is opened, and witnessed, on the next line) - but every quoted path in any
# NtCreateFile line is checked for where it points
OPEN_OK = re.compile(r'NtCreateFile (?:dir )?path="([^"]+)" -> handle=\d+')
OPEN_ANY = re.compile(r'NtCreateFile\b.*')
QUOTED = re.compile(r'"([^"]+)"')
XEX_LINE = re.compile(r"xex_loader: loaded \d+ bytes at VA 0x[0-9A-Fa-f]+ from (.+?) \(entry")
ROOT_LINE = re.compile(r'Content root: "([^"]+)"')
GLYPH_LINE = re.compile(r"\[GLYPH\] \d+ prompt icons, \d+ key glyphs from (\S.*?glyph_map\.txt)")
MEMMAP_LINE = re.compile(r"type=image .*?label=(.+)$")
SCOPES = ("exe image", "shader cache", "resolve kernels", "overlays", "content root", "disc opens", "game image",
          "DXC", "glyphs", "fonts")
NB_COMPILED = re.compile(r"\[NB_AOT\] compiled \S+ - no shipped blob")   # the staged blobs are not this exe's
NB_READY = "[NATIVE_RESOLVE] ready: 7 resolve CS from the shipped blobs (SR_NB_AOT)"


def cmd_check(a):
    d = os.path.abspath(a.dir)
    drive = next((c for c in (d, os.path.join(d, "drive")) if os.path.isfile(os.path.join(c, "stage.json"))), None)
    if not drive:
        print("STAGE_CHECK NOT STAGED: no stage.json in %s or its drive\\ (the roll ran without --stage)" % d)
        return 2
    st = read_json(os.path.join(drive, "stage.json")) or {}
    stage, exe, croot = st.get("stage", "?"), st.get("exe", "?"), st.get("content_root", "?")
    exe_dir = os.path.dirname(exe)
    shader = st.get("shader_cache_dir")
    nb = st.get("nb_dir")
    scopes = st.get("scopes") or {}
    src_disc = os.path.normcase(os.path.join((st.get("source") or {}).get("content_root") or "?", DISC))
    wit = collections.defaultdict(int)     # scope -> positive witnesses (inside the stage)
    off = collections.defaultdict(list)    # scope -> lines naming a file outside the stage
    port = False

    def see(scope, path, base, line):
        if under(path, base):
            wit[scope] += 1
        else:
            off[scope].append(line.strip()[:240])

    attempts = sorted(glob.glob(os.path.join(drive, "attempt*")))
    if not attempts:
        off["exe image"].append("no attempt folder under %s" % drive)
    for att in attempts:
        prov = read_json(os.path.join(att, "provenance.json")) or {}
        env = prov.get("effectiveSrEnv") or {}
        port = port or env.get("SR_PORT_FEATURES") == "1"
        line = "provenance exePath=%s sha256=%s" % (prov.get("exePath"), prov.get("exeSha256"))
        if norm(prov.get("exePath") or "?") == norm(exe) and \
                (prov.get("exeSha256") or "").lower() == (st.get("exe_sha256") or "-").lower():
            wit["exe image"] += 1
        else:
            off["exe image"].append(line)
        if shader and norm(env.get("SR_SHADER_CACHE_DIR") or "?") != norm(shader):
            off["shader cache"].append("SR_SHADER_CACHE_DIR=%s" % env.get("SR_SHADER_CACHE_DIR"))
        if nb:   # configuration is required, but does not prove any kernel was loaded
            if norm(env.get("SR_NB_AOT_DIR") or "?") != norm(nb):
                off["resolve kernels"].append("SR_NB_AOT_DIR=%s" % env.get("SR_NB_AOT_DIR"))
            if env.get("SR_NB_AOT") == "0" or env.get("SR_NB_AOT_EMIT") == "1":
                off["resolve kernels"].append("AOT disabled or emitting: missing-blob diagnostics are suppressed")
        for log in [os.path.join(att, n) for n in sorted(os.listdir(att)) if re.fullmatch(r"run(_r\d+)?\.log", n)]:
            with open(log, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = SHADER_LINE.search(line)
                    if m and shader:
                        see("shader cache", m.group(1), os.path.dirname(shader), line)
                    if nb and NB_COMPILED.search(line):
                        off["resolve kernels"].append("compiled at boot (the staged blobs are not this exe's): "
                                                      + line.strip()[:200])
                    if nb and NB_READY in line:
                        wit["resolve kernels"] += 1
                    m = MOD_LINE.search(line)
                    if m:
                        see("overlays", m.group(1), croot, line)
                        see("content root", m.group(1), croot, line)
                    m = ROOT_LINE.search(line)
                    if m:
                        see("content root", m.group(1), croot, line)
                    m = OPEN_OK.search(line)
                    if m and DISC.lower() in m.group(1).lower():
                        see("disc opens", m.group(1), croot, line)
                    elif OPEN_ANY.search(line):
                        for q in QUOTED.findall(line):        # a failed / alias open witnesses nothing, but a D:
                            if DISC.lower() in q.lower() and not under(q, croot):     # path is still a FAIL
                                off["disc opens"].append(line.strip()[:240])
                    m = XEX_LINE.search(line)
                    if m:
                        see("game image", m.group(1), croot, line)
                    m = GLYPH_LINE.search(line)
                    if m:
                        see("glyphs", m.group(1), exe_dir, line)
                    if DISC in line and src_disc in os.path.normcase(line):
                        off["disc opens"].append(line.strip()[:240])
    perf = os.path.join(os.path.dirname(drive), "perf")
    meta = os.path.join(perf, "meta.txt")
    if os.path.isfile(meta):
        with open(meta, encoding="utf-8", errors="replace") as f:
            exe_line = next((ln[4:].strip() for ln in f if ln.startswith("exe=")), "")
        see("exe image", exe_line, exe_dir, "perf/meta.txt exe=" + exe_line)
    for mm in sorted(glob.glob(os.path.join(perf, "memmap_*.txt"))):
        with open(mm, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = MEMMAP_LINE.search(line.rstrip())
                if not m:
                    continue
                base = os.path.basename(m.group(1)).lower()
                if base in ("dxcompiler.dll", "dxil.dll"):
                    see("DXC", m.group(1), exe_dir, line)
                elif base == os.path.basename(exe).lower():
                    see("exe image", m.group(1), exe_dir, line)
                elif base in ("sdl3.dll", "sdl2.dll", "ffmpeg_xenia.dll"):   # sdl2.dll: a pre-SDL3 exe
                    see("exe image", m.group(1), exe_dir, line)
    claimed = {"exe image": True, "shader cache": bool(shader), "resolve kernels": bool(nb),
               "overlays": port and scopes.get("port_features", True),
               "content root": True, "disc opens": True, "game image": True, "DXC": scopes.get("dxc", True),
               "glyphs": scopes.get("glyphs", True), "fonts": scopes.get("fonts", True)}
    how = {"exe image": "provenance / perf meta.txt / perf memmap", "shader cache": "[SHADER_CACHE] loaded ... from",
           "resolve kernels": "staged SR_NB_AOT_DIR plus shipped resolve readiness; no observed compile (not all variants)",
           "overlays": "[MOD] overlay adds (port features on)", "content root": "Content root (dev log) or overlays",
           "disc opens": "a successful NtCreateFile of a disc file (dev log)",
           "game image": "xex_loader: loaded ... from (dev log)",
           "DXC": "perf memmap module path (perf_run's SR_PERF_STATS)", "glyphs": "[GLYPH] ... from (dev log)",
           "fonts": "none: no log line names the font folder in any profile"}
    verdicts = {}
    for s in SCOPES:
        if not claimed[s]:
            verdicts[s] = "n/a"
        elif off[s]:
            verdicts[s] = "FAIL"
        elif wit[s]:
            verdicts[s] = "PASS"
        else:
            verdicts[s] = "INSUFFICIENT EVIDENCE"
        print("%-22s %-13s %3d witness(es)  [%s]" % (verdicts[s], s, wit[s], how[s]))
        for ln in off[s][:5]:
            print("        outside the stage: " + ln)
    fails = [s for s in SCOPES if verdicts[s] == "FAIL"]
    thin = [s for s in SCOPES if verdicts[s] == "INSUFFICIENT EVIDENCE"]
    if fails:
        print("STAGE_CHECK FAIL: %s" % ", ".join(fails))
        return 1
    if thin:
        print("STAGE_CHECK INSUFFICIENT EVIDENCE: %s (the dev log - perf_run --config port --gates SR_LOG=dev - "
              "witnesses all but fonts)" % ", ".join(thin))
        return 3
    print("STAGE_CHECK PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("plan", "stage"):
        p = sub.add_parser(name)
        p.add_argument("--stage", required=True, help="the SSD folder (e.g. J:\\sr_stage)")
        p.add_argument("--exe", required=True)
        p.add_argument("--root", default="", help="the game's working directory (default: the current directory)")
        p.add_argument("--source", default="", help="the folder holding '%s' to stage from" % DISC)
        p.add_argument("--floor-gib", type=float, default=FLOOR_GIB)
        p.add_argument("--max-gib", type=float, default=MAX_GIB)
        p.add_argument("--keep-exes", type=int, default=KEEP_EXES, help="exe folders kept, this one included "
                       "(0 = keep all)")
        p.add_argument("--lock-wait", type=float, default=600.0)
        if name == "plan":
            p.add_argument("--files", action="store_true", help="list every file with its size")
        else:
            p.add_argument("--json", default="", help="write the result here (sr_drive: <Out>/stage.json)")
            p.add_argument("--verify", choices=("full", "quick"), default="full",
                           help="full: re-hash every staged file before reuse (default); quick: trust a staged file "
                                "whose size and mtime match its manifest")
            p.add_argument("--lease-id", default="", help="hold the selected entries under this id until released")
            p.add_argument("--lease-pid", type=int, default=0, help="the lease holder (default: this process)")
            p.add_argument("--lease-ttl-h", type=float, default=LEASE_TTL_H)
            p.add_argument("--lease-note", default="")
    p = sub.add_parser("lease-add")
    p.add_argument("--stage", required=True)
    p.add_argument("--lease-id", required=True)
    p.add_argument("--pid", type=int, required=True, help="another holder: the lease lives while any holder runs")
    p = sub.add_parser("release")
    p.add_argument("--stage", required=True)
    p.add_argument("--lease-id", required=True)
    p.add_argument("--handoff-pid", type=int, default=0, help="keep the lease for this process only")
    p = sub.add_parser("status")
    p.add_argument("--stage", required=True)
    p = sub.add_parser("clean")
    p.add_argument("--stage", required=True)
    p.add_argument("--keep-exes", type=int, default=None, help="only retire exe folders beyond the N most recent")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--lock-wait", type=float, default=60.0)
    p = sub.add_parser("check")
    p.add_argument("dir", help="perf_run's --out (or sr_drive's -Out) of a staged roll")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "plan":
            return cmd_plan(a, dict(os.environ))
        if a.cmd == "stage":
            return cmd_stage(a, dict(os.environ))
        if a.cmd == "release":
            return cmd_release(a)
        if a.cmd == "lease-add":
            return cmd_lease_add(a)
        if a.cmd == "status":
            return cmd_status(a)
        if a.cmd == "clean":
            return cmd_clean(a)
        return cmd_check(a)
    except Refused as e:
        print("REFUSED: %s" % e, flush=True)
        return 2
    except (OSError, RuntimeError) as e:
        print("ERROR: %s" % e, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
