#!/usr/bin/env python3
"""build_custom_frontend.py - build an Apt content mod for the kernel asset overlay
(docs/MODS.md; CUSTOM_FRONTEND.md sections 7.2-7.3). The default mod is custom_frontend.

  1. reads the user's own ui.AST (read-only; default "<repo>/Superman Returns (USA)/DATA/ui.AST")
  2. applies every *.patch in the patch directory, in sorted file-name order (patch
     language: tools/apt/patch_movie.py; code bodies may live in .aptasm files that a
     patch names with file=...). Default directory: tools/apt/mods/<mod>/
  3. verifies from the final container bytes: same TOC (names, order, count); every
     entry that no patch names is byte-identical; every patched movie re-parses with 0
     uncovered non-zero bytes and round-trips through apt_graph; EVERY movie of the output
     meets the runtime's unload contract (tools/apt/apt_unload.py: each Push/ConstantPool
     slot holds its unload-walk position, one .const item per slot, a reload after unload
     resolves the same constants - else a movie works once and breaks on its next load);
     with no patches the whole output is byte-identical to the input
  4. writes the delivery form into <content root>/mods/<mod>/ (content root = the parent
     of build/, i.e. the repo root when run as documented; /mods/ is gitignored), then
     re-reads each file and compares it with what it meant to write:
       --form a (default)  DATA/ui.AST - the whole rebuilt container
       --form b            DATA/_workinprogress/<MOVIE>.ast per patched movie (the
                           engine's loose-file mount; experiment E1 decides if it works)
     Outputs of the other form left from an earlier build are removed first, so a
     stale file can never shadow the new one.
  5. --enable adds <mod> to <content root>/mods/enabled.txt (the user's switch; the exe
     reads it at boot - docs/MODS.md).
  6. (iter-464, custom_frontend only) the OPTION FOLDER @autosave_without_asking/: the disc's
     DATA/baseworl.AST with one same-length Lua edit (autosave_lua below). The exe mounts an
     option folder only while that player option is 1 at launch (kernel/mod_overlay.cpp,
     settings/mod_options.ini), so with the setting off the game reads the disc's file.
It never writes inside the game folder.

usage (repo root):
  python tools/apt/build_custom_frontend.py [--mod NAME] [--form a|b] [--enable]
         [--ui UI] [--patches DIR] [--content-root DIR] [--out FILE] [--check]
  --check   build and verify in memory, write nothing
"""
import argparse
import glob
import hashlib
import os
import re
import sys

try:
    from .bgfa import Bgfa, DEFAULT_UI, DATA_DIR, REPO, TAG_APT, TAG_CONST
    from .apt_model import Apt, coverage
    from .apt_graph import from_apt
    from .patch_movie import load_patch, patch_container, PatchError, refuse_game_folder
    from . import aptasm, apt_unload
except ImportError:  # run as a script from tools/apt (its dir is sys.path[0])
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bgfa import Bgfa, DEFAULT_UI, DATA_DIR, REPO, TAG_APT, TAG_CONST
    from apt_model import Apt, coverage
    from apt_graph import from_apt
    from patch_movie import load_patch, patch_container, PatchError, refuse_game_folder
    import aptasm
    import apt_unload

MODS_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mods")

# ---- iter-464: "Autosave without asking" - the end-of-chapter save prompt ----------------
# BaseWorldFlow's trigger-reactivation script (the state-105 enter Lua, baseworl.AST) advances
# the chapter and calls AutoSaveGame() - which in retail only captures the in-memory Retry
# checkpoint; nothing reaches the disk unless the player picks a slot. The mod puts the game's
# own PromptUserSaveGame() (ScriptCmd_PromptUserSaveGame_sub_822B5D78, the call the game makes
# after winning MSTobj03) in place of the debug print that follows AutoSaveGame(), so every
# chapter advance raises the game's own "SAVE GAME BEFORE CONTINUING?" prompt, which the
# custom front end answers (MAIN_HUD srautosave.aptasm: Save Game -> the current slot). The
# edit keeps the asset's length (no container offset, size or TOC field changes): the
# statement is replaced and the rest of the line becomes a Lua comment.
AUTOSAVE_OPTION = "autosave_without_asking"
AUTOSAVE_LUA_OLD = b'LuaDebugPrint("[base] you advanced to chapter " .. currentHeroPointChapter + 1)'
AUTOSAVE_LUA_NEW_HEAD = b"PromptUserSaveGame() -- sr custom_frontend: autosave"


def autosave_lua(baseworl):
    """the disc's baseworl.AST -> the edited bytes (same length). Raises PatchError."""
    n = baseworl.count(AUTOSAVE_LUA_OLD)
    if n != 1:
        raise PatchError(f"baseworl.AST: the chapter-advance debug print occurs {n} times (expected 1)")
    pad = len(AUTOSAVE_LUA_OLD) - len(AUTOSAVE_LUA_NEW_HEAD)
    new = AUTOSAVE_LUA_NEW_HEAD + b" " * pad
    assert len(new) == len(AUTOSAVE_LUA_OLD)
    at = baseworl.index(AUTOSAVE_LUA_OLD)
    # the line must be the advance branch right after AutoSaveGame()
    before = baseworl[max(0, at - 200):at]
    if b"AutoSaveGame()" not in before:
        raise PatchError("baseworl.AST: the debug print does not follow AutoSaveGame()")
    out = baseworl[:at] + new + baseworl[at + len(new):]
    if len(out) != len(baseworl):
        raise PatchError("baseworl.AST: the edit changed the length")
    return out


def option_folder_path(content_root, mod, option, rel):
    return os.path.join(mod_dir(content_root, mod), "@" + option, rel)
DEFAULT_MOD = "custom_frontend"
DEFAULT_PATCHES = os.path.join(MODS_SRC, DEFAULT_MOD)


def valid_mod_name(mod):
    """the exe's rule (kernel/mod_overlay.cpp validModName): 1-64 chars of [A-Za-z0-9_.-],
    not starting with '.'. A name outside it is skipped at boot with only a log line."""
    return (isinstance(mod, str) and 0 < len(mod) <= 64 and not mod.startswith(".")
            and all(c.isascii() and (c.isalnum() or c in "_.-") for c in mod))


def check_mod_name(mod):
    if not valid_mod_name(mod):
        raise PatchError(f"mod name {mod!r} is not a plain folder name: use 1-64 of A-Z a-z 0-9 _ . - "
                         f"and do not start with '.' (the game skips any other name; docs/MODS.md)")


def mod_dir(content_root, mod):
    return os.path.join(content_root, "mods", mod)


def form_a_path(content_root, mod):
    return os.path.join(mod_dir(content_root, mod), "DATA", "ui.AST")


def form_b_dir(content_root, mod):
    return os.path.join(mod_dir(content_root, mod), "DATA", "_workinprogress")


def enable_mod(content_root, mod, log=print):
    """add `mod` to <content root>/mods/enabled.txt (created with a header) unless listed."""
    check_mod_name(mod)
    path = os.path.join(content_root, "mods", "enabled.txt")
    lines = open(path, encoding="utf-8-sig").read().splitlines() if os.path.exists(path) else []
    names = [l.split("#", 1)[0].strip().lower() for l in lines]
    if mod.lower() in names:
        log(f"{mod} is already enabled in {path}")
        return path
    if not lines:
        lines = ["# Mods the game mounts at boot, one folder name (under mods/) per line.",
                 "# Earlier lines win when two mods provide the same file. '#' starts a comment.",
                 "# Remove or comment out a line to switch that mod off. See docs/MODS.md."]
    lines.append(mod)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    log(f"enabled {mod} in {path}")
    return path


def sha(b):
    return hashlib.sha256(b).hexdigest()[:16]


def collect(patch_dir):
    """-> (sections, patchFiles, unreferencedAptasm)."""
    if not os.path.isdir(patch_dir):
        return [], [], []
    files = sorted(glob.glob(os.path.join(patch_dir, "*.patch")))
    sections, used = [], set()
    for f in files:
        secs = load_patch(f)
        for _, ops in secs:
            for op in ops:
                if getattr(op, "file", None):
                    used.add(os.path.normcase(os.path.abspath(op.file)))
                for inc in getattr(op, "includes", []):
                    used.add(os.path.normcase(os.path.abspath(inc)))
        sections.extend(secs)
    asm = [a for a in sorted(glob.glob(os.path.join(patch_dir, "*.aptasm")))
           if os.path.normcase(os.path.abspath(a)) not in used]
    return sections, files, asm


def _code_lines(path):
    return [ln.split(";", 1)[0].strip() for ln in open(path, encoding="utf-8").read().splitlines()
            if ln.split(";", 1)[0].strip()]


# iter-464: every mod object, the files that bind its methods, and where they are deleted again.
# A method is bound by `Push [reg1]` / `EA_PushConstByte 'NAME'` / `.func2` or `.include` at the
# top level of a bind file (reg1 = the object there); it is deleted either by the unload snippet
# (`EA_GetNamedMember 'OBJ'` / `EA_PushConstByte 'NAME'` / `Delete`) or, for objects with a
# Teardown method, inside Teardown (`Push [reg1]` / 'NAME' / `Delete`, reg1 = this) - Teardown
# itself is then deleted by the unload snippet after it returned.
LIFETIME = [
    # (object, bind files, unload snippet)
    ("SRFrontEnd", ("srfrontend.aptasm", "modopts_methods.aptasm"), "exitscreen_head.aptasm"),
    ("SRDevTools", ("devtools_ui.aptasm",), "exitscreen_head.aptasm"),
    ("SRDevTools", ("devtools_ui.aptasm",), "pause_exitscreen_head.aptasm"),
    ("SRPause", ("srpause.aptasm", "modopts_methods.aptasm"), "pause_exitscreen_head.aptasm"),
    ("SRAutoSave", ("srautosave.aptasm", "modopts_methods.aptasm"), "hud_exitscreen_head.aptasm"),
]


def check_lifetime(patch_dir):
    """custom_frontend lifetime rule: every METHOD a mod object's bind files bind is deleted again
    when the movie that bound it unloads (LIFETIME above), so no function object of a movie
    outlives it (rolls quit_on1 / quit_v3: surviving methods = no reload request after QUIT GAME).
    -> a log line, or None when no listed bind file binds a method (another mod, or the rt_patch
    fixture). Raises PatchError."""

    def triples(lines, first, last):
        out = []
        for i in range(1, len(lines) - 1):
            m = re.match(r"EA_PushConstByte '([^']+)'$", lines[i])
            if m and lines[i - 1] == first and last(lines[i + 1]):
                out.append(m.group(1))
        return out

    notes = []
    for obj, binds, unload in LIFETIME:
        paths = [os.path.join(patch_dir, f) for f in binds]
        if not os.path.isfile(paths[0]):
            continue
        bound, in_teardown = [], []
        for pth in paths:
            if os.path.isfile(pth):
                lines = _code_lines(pth)
                bound += triples(lines, "Push [reg1]", lambda l: l.startswith((".func2", ".include")))
                in_teardown += triples(lines, "Push [reg1]", lambda l: l == "Delete")
        if not bound:
            continue
        ex = os.path.join(patch_dir, unload)
        deleted = triples(_code_lines(ex), f"EA_GetNamedMember '{obj}'", lambda l: l == "Delete") \
            if os.path.isfile(ex) else []
        if "Teardown" in bound:
            if "Teardown" not in deleted:
                raise PatchError(f"lifetime rule: {unload} never deletes {obj}.Teardown")
            deleted = deleted + in_teardown
        if sorted(bound) != sorted(deleted):
            raise PatchError(f"lifetime rule: {obj} methods bound {sorted(set(bound) - set(deleted))} are not deleted "
                             f"(by {unload} / Teardown), deleted but not bound {sorted(set(deleted) - set(bound))}")
        notes.append(f"{obj} {len(bound)} ({unload})")
    if not notes:
        return None
    return "lifetime rule: every bound method is deleted on unload: " + ", ".join(notes)


def verify_container(ui, new_ui, patched_names):
    """independent re-check of the final bytes; raises PatchError."""
    o1, o2 = Bgfa(ui), Bgfa(new_ui)
    if [e["name"] for e in o1.entries] != [e["name"] for e in o2.entries]:
        raise PatchError("output TOC names/order differ from the input")
    if [e["id"] for e in o1.entries] != [e["id"] for e in o2.entries]:
        raise PatchError("output TOC ids differ from the input")
    same = 0
    for e1, e2 in zip(o1.entries, o2.entries):
        if e1["name"].upper() in patched_names:
            inner = Bgfa(e2["data"])
            apt = next(x for x in inner.entries if x["tag"] == TAG_APT)["data"]
            cst = next(x for x in inner.entries if x["tag"] == TAG_CONST)["data"]
            a = Apt(apt, cst)
            if coverage(a)[3]:
                raise PatchError(f"{e1['name']}: output movie has uncovered non-zero bytes")
            tail = len(apt) - max(o + s for o, s, k in a.blocks)
            b, c, _ = from_apt(a).emit(orig_tail=tail)
            if (b, c) != (apt, cst):
                raise PatchError(f"{e1['name']}: output movie does not round-trip")
            others = [(x["tag"], x["id"], x["data"]) for x in Bgfa(e1["data"]).entries if x["tag"] not in (TAG_APT, TAG_CONST)]
            others2 = [(x["tag"], x["id"], x["data"]) for x in inner.entries if x["tag"] not in (TAG_APT, TAG_CONST)]
            if others != others2:
                raise PatchError(f"{e1['name']}: a non-.apt/.const asset of the movie changed")
        elif e1["data"] == e2["data"]:
            same += 1
        else:
            raise PatchError(f"{e1['name']}: changed although no patch names it")
    # the unload check here has no pre-renumbering constants, so its reload comparison adds
    # nothing once index == counter holds: its value is the index == counter / #slots ==
    # #items / all-slots-walked check on the FINAL bytes. The per-movie patch_movie.verify()
    # check 5 (with `before`) is what proves every slot still resolves to the same constant.
    n, fails = apt_unload.check_container(new_ui)
    if fails:
        raise PatchError(f"{len(fails)}/{n} output movie(s) would not survive the runtime's unload + reload: "
                         + "; ".join(f"{nm}: {' | '.join(pr)}" for nm, pr, _ in fails[:5]))
    return same, len(o1.entries), n


def _write_checked(path, data, log):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    if open(path, "rb").read() != data:
        raise PatchError(f"re-read of {path} differs from what was written")
    log(f"wrote {path} ({len(data)} B, re-read OK)")


def build(ui_path=DEFAULT_UI, patch_dir=DEFAULT_PATCHES, out_path=None, content_root=REPO, write=True, log=print,
          mod=DEFAULT_MOD, form="a"):
    check_mod_name(mod)
    if out_path:  # fail before the build: never overwrite the disc's (or --ui's) ui.AST
        refuse_game_folder(out_path, ui_path)
    ui = open(ui_path, "rb").read()
    sections, files, unref = collect(patch_dir)
    lifetime = check_lifetime(patch_dir)
    if not os.path.isdir(patch_dir):
        log(f"note: no patch directory {patch_dir}: building the identity mod")
    for a in unref:
        log(f"warning: {os.path.basename(a)} is not referenced by any patch (file=...)")
    new_ui, patchers = patch_container(ui, sections)
    patched = {p.name for p in patchers}
    for p in patchers:
        log(f"== {p.name} ({len(p.results)} ops)")
        for line in p.report:
            log("   " + line)
        for w in p.warnings:
            log("   warning: " + w)
        for c in p.checks:
            log("   [ok] " + c)
    same, total, n_movies = verify_container(ui, new_ui, patched)
    if not patched and new_ui != ui:
        raise PatchError("empty patch set but the output differs from the input")
    log(f"patch files: {len(files)}; movies patched: {len(patched)} {sorted(patched)}; "
        f"untouched TOC entries byte-identical: {same}/{total - len(patched)}")
    if lifetime:
        log(lifetime)
    log(f"unload contract (apt_unload: slot index == unload counter, slots == items, reload resolves "
        f"the first load's constants): {n_movies}/{n_movies} movies")
    log(f"ui.AST in  {len(ui)} B sha256 {sha(ui)}")
    log(f"ui.AST out {len(new_ui)} B sha256 {sha(new_ui)}" + ("  (byte-identical)" if new_ui == ui else ""))

    def guard(path):  # the same rule as patch_movie --out (symlinks/junctions resolved)
        refuse_game_folder(path, ui_path)

    outputs = []  # (path, bytes)
    if form == "a":
        outputs.append((out_path or form_a_path(content_root, mod), new_ui))
    elif form == "b":
        if out_path:
            raise PatchError("--out applies to form a only")
        if not patched:
            raise PatchError("form b needs at least one patched movie (it writes one file per patched movie)")
        for e in Bgfa(new_ui).entries:
            if e["name"].upper() in patched:
                outputs.append((os.path.join(form_b_dir(content_root, mod), e["name"].upper() + ".ast"), e["data"]))
    else:
        raise PatchError(f"unknown form {form!r} (a or b)")
    if os.path.isfile(os.path.join(patch_dir, "srautosave.aptasm")) and form == "a" and not out_path:
        bw_path = os.path.join(os.path.dirname(os.path.abspath(ui_path)), "baseworl.AST")
        bw = open(bw_path, "rb").read()
        bw2 = autosave_lua(bw)
        log(f"autosave option folder: baseworl.AST {len(bw)} B, 1 same-length Lua edit "
            f"(PromptUserSaveGame after the chapter advance), sha256 {sha(bw)} -> {sha(bw2)}")
        outputs.append((option_folder_path(content_root, mod, AUTOSAVE_OPTION, os.path.join("DATA", "baseworl.AST")), bw2))
    manifest = os.path.join(patch_dir, "mod.json")
    if os.path.isfile(manifest) and form == "a" and not out_path:
        # iter-464 modmenu: the mod's manifest (docs/MODS.md "The MODS menu"): name, author, version,
        # description; "port_feature": true keeps a port feature out of the player's MODS list
        outputs.append((os.path.join(mod_dir(content_root, mod), "mod.json"), open(manifest, "rb").read()))
        log(f"manifest: mod.json ({os.path.getsize(manifest)} B)")
    for path, _ in outputs:
        guard(path)
    if write:
        if out_path is None:
            # a stale output of the other form (or an old movie) must never shadow this build
            stale = [form_a_path(content_root, mod)] if form == "b" else []
            bdir = form_b_dir(content_root, mod)
            if os.path.isdir(bdir):
                names = {e["name"].upper() for e in Bgfa(new_ui).entries}
                stale += [os.path.join(bdir, f) for f in os.listdir(bdir)
                          if f.lower().endswith(".ast") and os.path.splitext(f)[0].upper() in names]
            keep = {os.path.normcase(os.path.abspath(p)) for p, _ in outputs}
            for f in stale:
                if os.path.isfile(f) and os.path.normcase(os.path.abspath(f)) not in keep:
                    os.remove(f)
                    log(f"removed stale {f}")
        for path, data in outputs:
            _write_checked(path, data, log)
    return new_ui, patchers


def main(argv):
    ap = argparse.ArgumentParser(description="Build an Apt content mod into <content root>/mods/<mod>/.")
    ap.add_argument("--mod", default=DEFAULT_MOD, help="mod name = output folder mods/<mod>/ (default custom_frontend)")
    ap.add_argument("--form", default="a", choices=["a", "b"],
                    help="a: whole DATA/ui.AST (default); b: DATA/_workinprogress/<MOVIE>.ast per patched movie")
    ap.add_argument("--enable", action="store_true", help="also add the mod to <content root>/mods/enabled.txt")
    ap.add_argument("--ui", default=DEFAULT_UI, help="source ui.AST (read-only)")
    ap.add_argument("--patches", help="directory of *.patch / *.aptasm (default tools/apt/mods/<mod>/)")
    ap.add_argument("--content-root", default=REPO, help="the output goes to <root>/mods/<mod>/")
    ap.add_argument("--out", help="explicit output file for form a (overrides --content-root)")
    ap.add_argument("--check", action="store_true", help="build and verify, write nothing")
    args = ap.parse_args(argv)
    patches = args.patches or os.path.join(MODS_SRC, args.mod)
    try:
        build(args.ui, patches, args.out, args.content_root, write=not args.check, mod=args.mod, form=args.form)
        if args.enable and not args.check:
            enable_mod(args.content_root, args.mod)
    except (PatchError, aptasm.AsmError) as ex:
        print(f"build_custom_frontend: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
