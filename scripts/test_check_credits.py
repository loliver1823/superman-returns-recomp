#!/usr/bin/env python3
"""test_check_credits.py - fixtures and mutants for scripts/check_credits.py.

check_credits.py is the only thing that keeps CREDITS.md / THIRD_PARTY_LICENSES.md from going
stale. A checker that always says OK is worse than none, so this test proves two things:
  * the real tree passes (exit 0);
  * every detector FIRES on a mutant: each case adds one fake tracked file or edits one input
    in memory (git_files / read_text are patched; the repo and the index are never touched)
    and must make the checker exit 1 with the expected category. Covered mutants must exit 0.

    python scripts/test_check_credits.py      # exit 0 = baseline OK and every mutant detected
"""
import contextlib
import importlib.util
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("check_credits", os.path.join(HERE, "check_credits.py"))
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

REAL_FILES = cc.git_files
REAL_READ = cc.read_text


def run(extra_files=None, overrides=None, drop_files=()):
    """Run check_credits.main with fake tracked files / file contents. Returns (exit, output)."""
    extra_files = extra_files or {}
    overrides = dict(overrides or {})
    overrides.update(extra_files)

    def fake_files():
        return [f for f in REAL_FILES() if f not in drop_files] + list(extra_files)

    def fake_read(rel, max_lines=None):
        if rel in overrides:
            txt = overrides[rel]
            if max_lines is not None:
                txt = "".join(txt.splitlines(True)[:max_lines])
            return txt
        return REAL_READ(rel, max_lines)

    cc.git_files, cc.read_text = fake_files, fake_read
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rc = cc.main(["--quiet"])
    finally:
        cc.git_files, cc.read_text = REAL_FILES, REAL_READ
    return rc, buf.getvalue()


def edit(rel, old, new):
    txt = REAL_READ(rel)
    assert old in txt, "fixture anchor missing in %s: %r" % (rel, old)
    return {rel: txt.replace(old, new, 1)}


CASES = [
    # (name, kwargs, expected exit, category that must appear in the output when failing)
    ("baseline: the real tree", {}, 0, None),
    ("vendored file under a third_party/ segment",
     {"extra_files": {"kernel/third_party/zlib/inflate.c": "int x;\n"}}, 1, "vendored"),
    ("new tree carrying its own LICENSE",
     {"extra_files": {"tools/newlib/LICENSE": "MIT License\n", "tools/newlib/a.py": "x = 1\n"}},
     1, "license-file"),
    ("attribution header: ported from Xenia, file not in the xenia entry",
     {"extra_files": {"kernel/new_port.cpp": "// Ported from Xenia Canary (BSD-3-Clause), src/xenia/foo.cc\nint y;\n"}},
     1, "attribution"),
    ("attribution header: Copyright line",
     {"extra_files": {"gfx/copied.hpp": "/* Copyright (c) 2019 Somebody */\n#pragma once\n"}},
     1, "attribution"),
    ("include from an untracked reference tree",
     {"extra_files": {"gfx/newpass.cpp": '#include "../reference/xenia-canary/src/xenia/gpu/x.h"\n'}},
     1, "external-include"),
    ("include of a TRACKED vendored header is covered through that header",
     {"extra_files": {"gfx/newhash.cpp": '#include "thirdparty/xxhash.h"\n'}}, 0, None),
    ("shipped asset outside any entry",
     {"extra_files": {"assets/ui/arrow.png": "PNG"}}, 1, "asset"),
    ("font file anywhere",
     {"extra_files": {"gfx/fonts_x/ui.ttf": "TTF"}}, 1, "asset"),
    ("glyph under data/glyphs is covered by the Julio Cacko entry",
     {"extra_files": {"data/glyphs/xbox/a.png": "PNG"}}, 0, None),
    ("new submodule",
     {"overrides": edit(".gitmodules", "[submodule \"third_party/vcpkg\"]",
                        "[submodule \"third_party/imgui\"]\n\tpath = third_party/imgui\n\turl = x\n"
                        "[submodule \"third_party/vcpkg\"]")}, 1, "submodule"),
    ("new vcpkg dependency",
     {"overrides": edit("vcpkg.json", '"sdl3"', '"sdl3", "zlib"')}, 1, "vcpkg"),
    ("new find_package",
     {"overrides": edit("CMakeLists.txt", "find_package(SDL3 3.4.18 CONFIG REQUIRED)",
                        "find_package(SDL3 3.4.18 CONFIG REQUIRED)\nfind_package(zstd CONFIG REQUIRED)")},
     1, "cmake-package"),
    ("CREDITS.md loses an entry marker",
     {"overrides": edit("CREDITS.md", "<!-- credits:xxhash -->", "")}, 1, "docs"),
    ("THIRD_PARTY_LICENSES.md loses a shipped entry marker",
     {"overrides": edit("THIRD_PARTY_LICENSES.md", "<!-- credits:sdl3 -->", "")}, 1, "docs"),
    ("THIRD_PARTY_LICENSES.md stops naming a licence file the build copies",
     {"overrides": {"THIRD_PARTY_LICENSES.md": REAL_READ("THIRD_PARTY_LICENSES.md").replace("LICENSE-fmt.txt", "LICENSE-fmt")}},
     1, "shipped-licence"),
    ("README stops linking CREDITS.md",
     {"overrides": {"README.md": REAL_READ("README.md").replace("CREDITS.md", "CREDITS")}}, 1, "docs"),
]


def main():
    failed = 0
    for name, kw, want, cat in CASES:
        rc, out = run(**kw)
        ok = rc == want and (cat is None or ("  " + cat) in out)
        print("%-4s %s (exit %d, want %d%s)" % ("ok" if ok else "FAIL", name, rc, want,
                                               ", category %s" % cat if cat else ""))
        if not ok:
            failed += 1
            print("     " + out.replace("\n", "\n     ")[:1500])
    print("%d case(s), %d failed" % (len(CASES), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
