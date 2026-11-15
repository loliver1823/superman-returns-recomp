#!/usr/bin/env python3
"""check_credits.py - keep CREDITS.md / THIRD_PARTY_LICENSES.md from going stale.

Source of truth: data/credits.json. This script finds everything in the TRACKED tree that came
from somebody else and fails (exit 1) when an item has no credits entry. Rule (docs/README.md):

    Port code, vendor a library, add a dependency, ship a binary or add an asset
    -> add or extend an entry in data/credits.json IN THE SAME COMMIT.

What it detects (static, git ls-files based, < 1 s):
  submodule        every path in .gitmodules
  vcpkg            every dependency in vcpkg.json
  cmake-package    every find_package(<Name>) in CMakeLists.txt / cmake/*.cmake / guest/CMakeLists.txt
  license-file     every tracked LICENSE* / COPYING* / NOTICE* file
  vendored         every tracked file inside a tree that carries its own LICENSE/COPYING, or under a
                   third_party/ thirdparty/ vendor/ external/ path segment
  attribution      every tracked source file whose first 60 lines say "ported from", "port of
                   <project>", "Copyright ...", "SPDX-License-Identifier", "Released under the", a
                   licence name (BSD/MIT/LGPL/GPL/Apache/zlib/CC0) or "based on/adapted from <project>"
  external-include every #include that reaches into reference/, third_party/, a thirdparty/ dir or a
                   recompiler checkout (code compiled in from an untracked tree)
  asset            every tracked file under a shipped asset root (data/glyphs/, assets/, fonts/,
                   resources/) and every font file anywhere
  shipped-licence  every LICENSE-*.txt/.md the CMake build copies next to the exe must be named in
                   THIRD_PARTY_LICENSES.md
  docs             every entry id must appear as <!-- credits:ID --> in CREDITS.md, every entry that
                   ships must appear the same way in THIRD_PARTY_LICENSES.md, README.md must link
                   CREDITS.md

An item is covered when a credits entry's "paths" glob matches it ("**" crosses directories, "*"
does not), its "submodule"/"vcpkg"/"cmake_packages" names it, or "not_third_party" lists it with
a reason (our own file that merely mentions a licence).

Usage:
  python scripts/check_credits.py              # check; exit 0 ok, 1 uncovered items, 2 bad input
  python scripts/check_credits.py --report     # also print what each entry covers
  python scripts/check_credits.py --game-data  # census of tracked game-derived files (info only)
  python scripts/check_credits.py --quiet      # print failures only
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CREDITS_JSON = "data/credits.json"
CREDITS_MD = "CREDITS.md"
LICENSES_MD = "THIRD_PARTY_LICENSES.md"
README_MD = "README.md"

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".hlsl", ".hlsli",
              ".xesli", ".py", ".js", ".ts", ".ps1", ".psm1", ".java", ".cmake", ".patch",
              ".diff", ".bat", ".cmd", ".sh", ".toml", ".cs", ".lua", ".aptasm")
HEADER_LINES = 60
SKIP_PREFIXES = ("docs/", "data/", "archive/", ".claude/", ".duet/", "director/")
VENDOR_SEGMENTS = {"third_party", "thirdparty", "vendor", "external", "extern"}
ASSET_ROOTS = ("data/glyphs/", "assets/", "fonts/", "resources/")
FONT_EXT = (".ttf", ".otf", ".woff", ".woff2", ".fnt", ".bdf", ".pcf")
LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|notice|copyright|unlicense)([-_.].*)?$", re.I)
TREE_LICENSE_NAME = re.compile(r"^(licen[cs]e|copying)(\.(txt|md|rst))?$", re.I)

PROJECTS = (r"xenia|xenonrecomp|xenosrecomp|unleashed|rexglue|opensage|ffmpeg|libertyv|xma2dec|"
            r"sdl|xxhash|simde|fmt\b|dxc|directxshadercompiler|extract-xiso|xdvdfs|n64recomp|"
            r"ghidra|imgui|stb_|lua")
ATTRIBUTION = re.compile(
    r"\bported\s+(from|semantics)\b"
    r"|\bport\s+of\s+(" + PROJECTS + r")"
    r"|\bcopyright\s*(\(c\)|©|\d{4})"
    r"|SPDX-License-Identifier"
    r"|\breleased under the\b"
    r"|\b(BSD|MIT|LGPL|GPL|Apache|zlib|CC0)[- ]?(\d[\w.-]*|licen[cs]e|clause)"
    r"|\b(based on|adapted from|derived from|taken from|copied (verbatim )?from|verbatim from)\b"
    r"[^\n]{0,60}(" + PROJECTS + r"|github\.com)",
    re.I)
EXTERNAL_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"]([^>"]*(?:reference/|third_party/|thirdparty/|XenonRecomp|'
    r'XenosRecomp|UnleashedRecomp|xenia-canary)[^>"]*)[>"]', re.M)
FIND_PACKAGE = re.compile(r"^\s*find_package\s*\(\s*([A-Za-z0-9_.+-]+)", re.M | re.I)
SHIPPED_LICENSE = re.compile(r"LICENSE-[A-Za-z0-9_.+-]+\.(?:txt|md)")
DOC_MARK = "<!-- credits:{} -->"

KINDS = {"ported-code", "vendored", "submodule", "linked-library", "shipped-binary",
         "build-dependency", "asset", "reference", "tool", "game", "inspiration", "data-format"}
STATUSES = {"active", "incoming", "reference-only"}


def git_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True)
    return [f for f in out.stdout.decode("utf-8", "replace").split("\0") if f]


def read_text(rel, max_lines=None):
    p = os.path.join(ROOT, rel)
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as fh:
            if max_lines is None:
                return fh.read()
            lines = []
            for i, ln in enumerate(fh):
                if i >= max_lines:
                    break
                lines.append(ln)
            return "".join(lines)
    except OSError:
        return ""


_glob_cache = {}


def glob_re(pat):
    r = _glob_cache.get(pat)
    if r is None:
        out, i = "", 0
        while i < len(pat):
            c = pat[i]
            if pat.startswith("**", i):
                out += ".*"
                i += 2
                if i < len(pat) and pat[i] == "/":
                    out = out[:-2] + "(?:.*/)?"
                    i += 1
                continue
            out += "[^/]*" if c == "*" else "[^/]" if c == "?" else re.escape(c)
            i += 1
        r = _glob_cache[pat] = re.compile("^" + out + "$")
    return r


def submodules():
    txt = read_text(".gitmodules")
    return re.findall(r"^\s*path\s*=\s*(.+?)\s*$", txt, re.M)


def detect(files):
    """Return a list of (category, item, detail)."""
    found = []
    fileset = set(files)
    subs = submodules()
    for s in subs:
        found.append(("submodule", s, ".gitmodules"))
    try:
        vj = json.loads(read_text("vcpkg.json") or "{}")
    except ValueError:
        vj = {}
    for dep in vj.get("dependencies", []):
        name = dep if isinstance(dep, str) else dep.get("name", "?")
        found.append(("vcpkg", name, "vcpkg.json"))
    cmake_files = [f for f in files if f == "CMakeLists.txt" or (f.startswith("cmake/") and f.endswith(".cmake"))
                   or f == "guest/CMakeLists.txt"]
    pkgs = set()
    for cf in cmake_files:
        for m in FIND_PACKAGE.finditer(read_text(cf)):
            if m.group(1) not in ("Git", "Python", "Python3", "PythonInterp", "Threads"):
                pkgs.add((m.group(1), cf))
    for name, cf in sorted(pkgs):
        found.append(("cmake-package", name, cf))

    # trees that carry their own LICENSE/COPYING (vendored code)
    tree_roots = set()
    for f in files:
        base = f.rsplit("/", 1)[-1]
        if "/" in f and TREE_LICENSE_NAME.match(base):
            tree_roots.add(f.rsplit("/", 1)[0] + "/")
    sub_prefixes = tuple(s.rstrip("/") + "/" for s in subs)
    for f in files:
        if f in subs:
            continue  # gitlinks are reported as submodules
        base = f.rsplit("/", 1)[-1]
        if LICENSE_NAME.match(base):
            found.append(("license-file", f, "licence text"))
        segs = f.split("/")[:-1]
        if any(s.lower() in VENDOR_SEGMENTS for s in segs) or any(f.startswith(t) for t in tree_roots):
            if not LICENSE_NAME.match(base):
                found.append(("vendored", f, "inside a vendored tree"))
        if f.startswith(ASSET_ROOTS) or f.lower().endswith(FONT_EXT):
            found.append(("asset", f, "shipped asset root / font"))
        if f.startswith(SKIP_PREFIXES) or f.startswith(sub_prefixes):
            continue
        if not f.lower().endswith(SOURCE_EXT):
            continue
        if any(f.startswith(t) for t in tree_roots) or any(s.lower() in VENDOR_SEGMENTS for s in segs):
            continue  # already reported as vendored
        head = read_text(f, HEADER_LINES)
        m = ATTRIBUTION.search(head)
        if m:
            found.append(("attribution", f, " ".join(m.group(0).split())[:60]))
        if f.lower().endswith((".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".hlsl", ".hlsli")):
            body = read_text(f)
            incs = []
            for inc in sorted(set(EXTERNAL_INCLUDE.findall(body))):
                # an include that resolves to a TRACKED file is covered through that file (the
                # vendored/licence rules above); only code pulled from an untracked tree counts here
                cands = (os.path.normpath(os.path.join(os.path.dirname(f), inc)).replace("\\", "/"),
                         os.path.normpath(inc).replace("\\", "/"))
                if not any(c in fileset for c in cands):
                    incs.append(inc)
            if incs:
                found.append(("external-include", f, incs[0] + (" (+%d more)" % (len(incs) - 1) if len(incs) > 1 else "")))
    return found, cmake_files


def covers(entry, cat, item):
    if cat == "submodule":
        return item in entry.get("submodules", [])
    if cat == "vcpkg":
        return item in entry.get("vcpkg", [])
    if cat == "cmake-package":
        return item in entry.get("cmake_packages", [])
    return any(glob_re(p).match(item) for p in entry.get("paths", []))


def main(argv):
    report = "--report" in argv
    game_data = "--game-data" in argv
    quiet = "--quiet" in argv
    cj = os.path.join(ROOT, CREDITS_JSON)
    try:
        with open(cj, encoding="utf-8") as fh:
            credits = json.load(fh)
    except (OSError, ValueError) as e:
        print("check_credits: cannot read %s: %s" % (CREDITS_JSON, e))
        return 2
    entries = credits.get("entries", [])
    problems, warnings = [], []

    ids = set()
    for e in entries:
        eid = e.get("id", "?")
        for k in ("id", "name", "kind", "license", "url", "what", "status"):
            if not e.get(k):
                problems.append(("schema", eid, "missing field '%s'" % k))
        if eid in ids:
            problems.append(("schema", eid, "duplicate id"))
        ids.add(eid)
        if e.get("kind") not in KINDS:
            problems.append(("schema", eid, "kind '%s' not in %s" % (e.get("kind"), sorted(KINDS))))
        if e.get("status") not in STATUSES:
            problems.append(("schema", eid, "status '%s' not in %s" % (e.get("status"), sorted(STATUSES))))
    ntp = {x["path"]: x.get("reason", "") for x in credits.get("not_third_party", [])}
    for p, why in ntp.items():
        if not why:
            problems.append(("schema", p, "not_third_party entry needs a reason"))

    files = git_files()
    found, cmake_files = detect(files)
    covered_by = {}
    for cat, item, detail in found:
        hit = [e["id"] for e in entries if covers(e, cat, item)]
        if hit:
            covered_by.setdefault(hit[0], []).append((cat, item))
            continue
        if item in ntp and cat in ("attribution", "license-file"):
            continue
        problems.append((cat, item, detail))

    # globs that match nothing (stale paths) - warning, unless the entry is still incoming
    fileset = set(files)
    for e in entries:
        for p in e.get("paths", []):
            rx = glob_re(p)
            if not any(rx.match(f) for f in fileset):
                if e.get("status") != "incoming" and not e.get("untracked_paths_ok"):
                    warnings.append((e["id"], "path glob matches no tracked file: %s" % p))
        for lf in e.get("license_files", []):
            if lf.startswith("(") or "/" not in lf or " " in lf:
                continue  # annotated / out-of-tree / shipped-name entries are documentation only
            if not os.path.exists(os.path.join(ROOT, lf)):
                warnings.append((e["id"], "license_files entry not on disk: %s" % lf))

    # docs in sync
    cm = read_text(CREDITS_MD)
    lm = read_text(LICENSES_MD)
    rm = read_text(README_MD)
    if not cm:
        problems.append(("docs", CREDITS_MD, "missing"))
    if not lm:
        problems.append(("docs", LICENSES_MD, "missing"))
    for e in entries:
        if cm and DOC_MARK.format(e["id"]) not in cm:
            problems.append(("docs", e["id"], "no %s marker in %s" % (DOC_MARK.format(e["id"]), CREDITS_MD)))
        if lm and e.get("ships") and DOC_MARK.format(e["id"]) not in lm:
            problems.append(("docs", e["id"], "ships, but no %s marker in %s" % (DOC_MARK.format(e["id"]), LICENSES_MD)))
    if rm and "CREDITS.md" not in rm:
        problems.append(("docs", README_MD, "does not link CREDITS.md"))
    shipped = set()
    for cf in cmake_files:
        shipped.update(SHIPPED_LICENSE.findall(read_text(cf)))
    for name in sorted(shipped):
        if lm and name not in lm:
            problems.append(("shipped-licence", name, "copied next to the exe by CMake but not named in %s" % LICENSES_MD))

    if report:
        print("== coverage per entry")
        for e in entries:
            items = covered_by.get(e["id"], [])
            cats = {}
            for c, _ in items:
                cats[c] = cats.get(c, 0) + 1
            print("  %-22s %-16s %-14s %s" % (e["id"], e.get("license", ""), e.get("status", ""),
                                             ", ".join("%s=%d" % kv for kv in sorted(cats.items())) or "-"))
    if game_data:
        print("== tracked game-derived content (data/credits.json game_derived; info only)")
        for g in credits.get("game_derived", []):
            n = b = 0
            for p in g.get("paths", []):
                rx = glob_re(p)
                for f in fileset:
                    if rx.match(f):
                        n += 1
                        try:
                            b += os.path.getsize(os.path.join(ROOT, f))
                        except OSError:
                            pass
            print("  %6d files %12d bytes  %-12s %s" % (n, b, g.get("class", ""), ", ".join(g.get("paths", []))))

    if not quiet:
        for eid, w in warnings:
            print("warning: [%s] %s" % (eid, w))
    if problems:
        print("check_credits: %d uncovered / inconsistent item(s):" % len(problems))
        for cat, item, detail in problems:
            print("  %-16s %s  (%s)" % (cat, item, detail))
        print("Fix: add or extend an entry in %s (paths / submodules / vcpkg / cmake_packages), or list an"
              " own file under not_third_party with a reason; keep %s and %s in sync (docs/README.md"
              " 'Credits rule')." % (CREDITS_JSON, CREDITS_MD, LICENSES_MD))
        return 1
    if not quiet:
        print("check_credits: OK - %d detected items covered by %d entries (%d warnings)"
              % (len(found), len(entries), len(warnings)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
