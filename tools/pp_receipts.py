#!/usr/bin/env python3
"""pp_receipts.py - token-identity receipts for a source change that must not change a build (the C1 oracle splits).

SCOPE AND CLAIM
  Exit 0 means: for one ninja build dir, every C/C++ compile unit that can read a file changed since --base yields the
  SAME preprocessed token stream (`-E -P` with the unit's own compile command) with --base's files as with this tree's.
  Nothing else is claimed. Exit 2 (INCOMPLETE) whenever the run leaves that scope or cannot establish it:
    - compile edges: a compile edge is a compile-database entry whose command starts with a GNU-style C/C++ driver
      (clang, clang++, gcc, g++, cc, c++, with an optional target prefix / version suffix; the name is taken after
      resolving the path, so 8.3 names count; one ccache / sccache launcher in front is skipped). Each must have
      exactly one `-c` and one separate `-o`, a source ending .c .cc .cpp .cxx, `-x` only c / c++ / c-header /
      c++-header, no response file (@file), no module flags, no long-form dependency option (--write-dependencies,
      --write-user-dependencies, ...), no forwarded or driver-level option (-Wp,..., -Xpreprocessor, any -X... other
      than the accepted -Xclang forms, /clang:..., --config..., --driver-mode, -ccc-...; accepted: -Xclang followed by
      -emit-pch or --dependent-lib=..., and -Xclang -include / -include-pch -Xclang <path>), at most one non-empty
      -include-pch (clang uses the last of several; `-include-pch x.pch -include-pch ""` loads none), no option that
      shapes the -E output (-P, -C, -CC, -dM / -dD / -dI / -dN / -dU / -dE, -fdirectives-only, -frewrite-includes /
      -imports), and its own object. Out of
      scope (exit 2, never skipped): any other compile edge, an MSVC-style compiler (cl, clang-cl), a C/C++ source built
      without -c, and a C/C++ source built by a command that does not start with a compiler (a wrapper or script).
      Commands are split as the platform does (Windows: the C runtime's rules, with `""` inside quotes refused as
      ambiguous; elsewhere: POSIX sh quoting, with any shell metacharacter refused);
    - the change: `git diff` of tracked files (an untracked, not ignored file, an assume-unchanged / skip-worktree
      entry, a submodule, a symlink, two changed paths that differ only in case, a run outside the top level: exit 2);
      every changed file is read by a unit or named by --assume-no-codegen 'PATTERN=REASON' (recorded). A changed file
      that a non-compile build edge consumes - a declared input (through phony aliases), or a file in that edge's own
      deps record (e.g. the resource compiler) - is exit 2 even when units read it; so is a changed file inside a
      precompiled header a unit uses. Ignored files (generated inputs such as guest/ppc) are the same on both sides;
    - evidence: a unit's read set comes from its ninja deps record only when the record is VALID, complete, current,
      names the unit's source, was made with -MD (a -MMD / -MM record or an unknown deps mode omits system headers),
      and clang's own `-###` for the unit's command, read positionally, shows the driver's dependency group
      `-dependency-file F -MT T [-MT T ...] -sys-header-deps` (F and the targets are values, so `-I -sys-header-deps`
      or `-MT -sys-header-deps` is not the flag). A dependency token anywhere else in the job, a clang configuration
      file in use (clang.cfg / <triple>-clang++.cfg loaded on its own, or --config), a job whose precompiled header is
      not exactly the command's (several, empty, or one the driver adds for an -include x.h next to an x.h.pch), or a
      job line that cannot be parsed is exit 2; a group without the flag sends the unit to -M; another driver's
      records are never trusted.
      Otherwise the read set is the compiler's own -M, which must name the source (on clang its -M command must
      classify as full the same way); no read set for any unit is exit 2. A unit whose AFTER preprocessing reads a
      different file set from its build's deps record is FAILED (the receipt's environment resolves includes
      differently from the build's), with one explained exception - exactly the cause found in real use: clang's
      builtin stddef.h / stdarg.h probe the next header of their name (`__has_include_next(<stddef.h>)`, line 22)
      without entering it; a PCH's input files are only the files it ENTERED, so the probed UCRT stddef.h / MSVC
      stdarg.h is in the PCH unit's record but not in a consumer's -c record, while -E runs the PCH's header again and
      records the probe. A file is excused only if ALL hold: its PCH's trusted build record lists it; no line marker of
      the unit's plain -E pass names it (run without any output-shaping option, accepted only when its markers name the
      unit's own source); it lies under a system include directory of the unit's cc1 job (-internal-isystem /
      -isystem / -internal-externc-isystem in -###); and a header the unit DID enter, under the cc1 job's
      -resource-dir (a clang builtin header), contains `__has_include_next(<NAME>)` for its include name. Excused files
      are named in the read_set column; anything else is FAILED. Every unit's marker pass runs, with clang's cc1 -dE
      (`-Xclang -dE`: each #embed is printed as a canonical #embed line instead of expanded, so clang's own lexer finds
      every spelling); an embedded input leaves no marker, so no answer, a driver that cannot show embeds, or any
      #embed line is FAILED (a text scan of the entered files is only an extra check). A file deleted since --base
      gets its readers from every unit's BEFORE-side -M;
      with a new or deleted file, every compiler must record __has_include'd files in its dependency output (probed).
      Driver environment variables (CPATH, INCLUDE, CCC_OVERRIDE_OPTIONS, CLANG_NO_DEFAULT_CONFIG, ...) set in the
      tool's environment are exit 2 (the build's environment is seen only through its deps records);
    - tokens: string / char literals (u8 / u / U / L, raw R"d(...)d" with any legal delimiter) and their user-defined
      suffixes are single tokens, digraphs are tokens (`<::` as the standard splits it), whitespace outside literals is
      not a token, a #pragma line keeps its end of line. A non-ASCII character outside a literal, an undecodable byte
      (kept distinct inside literals), a line marker, a directive other than #pragma (#embed, ...), or a literal the
      lexer does not recognise is exit 2.
  Not compared at all: code generation, link, packaging, the compile commands themselves (both sides use the AFTER
  build's commands - a build-configuration change is out of scope, which is why unread changed files need the flag),
  a generator's undeclared inputs, a compiler hidden inside a script, and values that preprocessing does not fix
  (__builtin_LINE; __DATE__ / __TIME__ / __TIMESTAMP__ make units DIFF, never EQUAL).
  Exit 1 = some unit's streams differ (DIFF) or its source is new (NEW). There is no exception class: a difference that
  looks harmless (e.g. only integer literals) is DIFF, with an advisory note, and must be judged by review.

WHERE TO RUN: on the cloud worker (never build locally), in a synced tree whose build dir is built - the deps records,
the compile database and build.ninja come from that build; this tool never compiles an object:

    python tools/cloud/sr_cloud.py sync  --worktree . --name <tree>
    python tools/cloud/sr_cloud.py build --worktree . --name <tree>                       # default config: build/
    python tools/cloud/sr_cloud.py build --worktree . --name <tree> --build-dir build-rel --define SR_RELEASE_COMPOSITION=ON
    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/pp_receipts.py --build-dir build --base <ref> --out build/pp_default [--assume-no-codegen "docs/*.md=documentation"]
    python tools/cloud/sr_cloud.py run --name <tree> -- python tools/pp_receipts.py --build-dir build-rel --base <ref> --out build-rel/pp_rel [...]
    python tools/cloud/sr_cloud.py fetch --name <tree> build/pp_default/summary.tsv archive/<lane>/pp_default/
    (and diff_*.txt; the before/ after/ outputs are large - leave them on the worker)
    python tools/pp_receipts.py --self-test [--cc <clang++>]     # the scope checks and the attacks tried on it; --cc
                                                                 # adds the compiler-backed ones

INPUTS
  --build-dir DIR                the ninja build dir (build.ninja, the deps log, the compile database)
  --base REF                     the BEFORE ref: any commit in the repo (on the worker: an ancestor of the snapshot)
  --out DIR                      output folder (relative to the tree root; earlier outputs in it are removed first)
  --assume-no-codegen PAT=REASON a changed file (fnmatch on its repo path) no unit reads, stated to feed no compiled
                                 input; REASON required and recorded. A pattern matching nothing, or one naming a file
                                 a unit reads or a build edge consumes, is exit 2. Repeatable.
  --jobs N                       parallel compiler runs (default: CPU count)

HOW: (1) changed files = `git diff --no-renames --name-only -z <base>`; refuse a changed file that differs from HEAD (an
interrupted run's leftover). (2) units = the compile edges of `ninja -t compdb`, checked against the scope; read sets
as above; build.ninja's edges and the deps records of non-compile edges for the consumer check. (3) each affected unit
preprocessed for AFTER (with -MD, for the read-set cross-check), then for BEFORE (the changed files replaced by
`git cat-file --filters <base>:<path>` - exactly what a checkout writes - new ones moved aside, deleted ones written
back; always restored with their mtimes). (4) the two token streams compared.

OUTPUTS (in --out): summary.tsv - `#` lines (base, build dir, every changed file with kind and readers or reason,
coverage counts), then tu, object, before tokens, after tokens, result (EQUAL / DIFF / NEW / FAILED), read_set (ninja
deps, cross-checked against the AFTER preprocessing; or compiler -M and why), note (for DIFF: an advisory description
of the differing tokens - never an exemption); diff_<name>.txt for every DIFF unit (a display:
blank lines dropped); before/, after/ (<name>.i.txt and its .d). <name> = the object's file name + a hash of its path
(collision-checked). Exit: 0 = every affected unit EQUAL; 1 = some DIFF / NEW and none FAILED; 2 = INCOMPLETE, out of
scope, FAILED, or could not run.

Written for release/oracle-mixed-a (C1 oracle split); TOOLKIT 1.8j, docs/TOOLS_INDEX.md.
"""
import argparse
import difflib
import fnmatch
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import traceback
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from functools import lru_cache

TU_EXT = (".cpp", ".cc", ".cxx", ".c")
LANGS = ("c", "c++", "c-header", "c++-header")
EOL = "<EOL>"
LAUNCHERS = ("sccache", "sccache.exe", "ccache", "ccache.exe")
GNU_DRIVER = re.compile(r"^(?:[\w.+-]*-)?(?:clang\+\+|clang|gcc|g\+\+|c\+\+|cc)(?:-[\d.]+)?(?:\.exe)?$")
MSVC_DRIVER = re.compile(r"^(?:cl|clang-cl)(?:\.exe)?$")
PP_OUTPUT_FLAGS = ("-P", "-C", "-CC", "-dM", "-dD", "-dI", "-dN", "-dU", "-dE", "-fdirectives-only",
                   "-frewrite-includes", "-frewrite-imports")  # options that shape what -E writes (markers included)
MODULE_FLAGS = ("-fmodules", "-fmodule-file", "-fmodule-map-file", "-fprebuilt-module-path", "-fmodule-output",
                "--precompile", "-fpch-preprocess", "--output")
DEPS_ALIASES = ("--write-dependencies", "--write-user-dependencies", "--user-dependencies", "--dependencies",
                "--print-missing-file-dependencies")  # long forms of -MD / -MMD / -MM / -M / -MG
XCLANG_OK = ("-emit-pch",)  # -Xclang <one of these>: no effect on preprocessing or on the dependency output
XCLANG_OK_PREFIX = ("--dependent-lib=",)
XCLANG_PATH = ("-include", "-include-pch")  # -Xclang <one of these> -Xclang <path>: a forced include / the PCH
FORWARD_PREFIX = ("-X", "-Wp,", "/clang:", "--config", "--driver-mode", "-ccc-")
DRIVER_ENV = ("CCC_OVERRIDE_OPTIONS", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
              "OBJCPLUS_INCLUDE_PATH", "INCLUDE", "EXTERNAL_INCLUDE", "CL", "_CL_", "GCC_EXEC_PREFIX", "COMPILER_PATH",
              "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES", "CLANG_NO_DEFAULT_CONFIG")
CC1_DEPS = ("-dependency-file", "-MT", "-MQ", "-sys-header-deps")  # the cc1 tokens of the driver's dependency group
GIT = ["git", "-c", "core.fsmonitor=false", "-c", "core.quotePath=false", "-c", "diff.relative=false"]

UD = r"(?:[A-Za-z_]\w*)?"  # a user-defined literal suffix stays part of its literal
LEX = re.compile(r'''
    (?P<raw>(?:u8|u|U|L)?R"(?P<d>[!-'*-\[\]-~]{0,16})\(.*?\)(?P=d)"''' + UD + r''')
  | (?P<rawbad>(?:u8|u|U|L)?R")
  | (?P<str>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"''' + UD + r''')
  | (?P<chr>(?:u8|u|U|L)?'(?:\\.|[^'\\\n])+\'''' + UD + r''')
  | (?P<num>\.?\d(?:[eEpP][+-]|'\w|[\w.])*)
  | (?P<id>[A-Za-z_$][\w$]*)
  | (?P<lt_colons><(?=::[^:>]))
  | (?P<punct>%:%:|<=>|->\*|\.\.\.|<<=|>>=|::|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=
       |\#\#|\.\*|<:|:>|<%|%>|%:)
  | (?P<nl>\n)
  | (?P<ws>[ \t\r\f\v]+)
  | (?P<quote>["'])
  | (?P<other>.)
''', re.S | re.X | re.ASCII)
LINE_MARKER = re.compile(r"[ \t]*#[ \t]*(?:line[ \t]+)?\d")


class LexError(ValueError):
    pass


def tokens(text):
    """The token stream of `-E -P` output; LexError on anything outside the lexer's scope (see SCOPE)."""
    out, i, n = [], 0, len(text)
    bol, directive = True, False
    while i < n:
        if bol and LINE_MARKER.match(text, i):
            raise LexError("a line marker at line %d (the output of -P has none)" % (text.count("\n", 0, i) + 1))
        m = LEX.match(text, i)
        kind, tok = m.lastgroup, m.group(0)
        if kind in ("rawbad", "quote") or (kind == "other" and ord(tok) > 127):
            what = "a non-ASCII character outside a literal" if kind == "other" else "an unrecognised literal"
            raise LexError("%s at line %d: %r" % (what, text.count("\n", 0, i) + 1, text[i:i + 40]))
        i = m.end()
        if kind == "nl":
            if directive:
                if out[-1] in ("#", "%:"):
                    raise LexError("a null directive at line %d" % text.count("\n", 0, i))
                out.append(EOL)
            bol, directive = True, False
            continue
        if kind == "ws":
            continue
        if directive and out[-1] in ("#", "%:") and tok != "pragma":
            raise LexError("a #%s directive at line %d (only #pragma lines are expected after -E)" %
                           (tok, text.count("\n", 0, i) + 1))
        if bol and tok in ("#", "%:"):
            directive = True
        bol = False
        out.append(tok)
    if directive:
        if out[-1] in ("#", "%:"):
            raise LexError("a null directive at the end of the output")
        out.append(EOL)
    return out


def read_text(path):
    """A preprocessed file as text: UTF-8, an undecodable byte kept as a distinct surrogate (outside a literal it is
    non-ASCII, i.e. a LexError; inside one, two different bytes stay different)."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="surrogateescape")


def display_lines(text):
    return [l for l in text.splitlines() if l.strip()]


def advisory(ta, tb):
    """A description of how two differing streams differ - advice for the reviewer, never an exemption."""
    if len(ta) != len(tb):
        return "token counts differ (%d vs %d); DIFF - judge by review" % (len(ta), len(tb))
    pairs = [(x, y) for x, y in zip(ta, tb) if x != y]
    ints = all(x.isdigit() and y.isdigit() for x, y in pairs)
    return "%d token(s) differ%s; DIFF - judge by review" % (len(pairs), ", all of them integer literals" if ints
                                                                       else "")


def run(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)
    except OSError as ex:
        return subprocess.CompletedProcess(cmd, 127, "", str(ex))


def norm(p):
    return os.path.normcase(os.path.normpath(p)).replace("\\", "/")


@lru_cache(maxsize=None)
def canon(p):
    """One spelling per file: symlinks / junctions / 8.3 names resolved, case and separators normalized."""
    return norm(os.path.realpath(p))


def crt_split(s):
    """A Windows command line split as the C runtime (and Rust's std, which sccache uses) splits it; ValueError where
    C runtimes disagree (`""` inside quotes) or the line is malformed."""
    i, n = 0, len(s)
    while i < n and s[i] in " \t":
        i += 1
    if i >= n:
        return []
    if s[i] == '"':  # the program name: quotes only, no escapes
        j = s.find('"', i + 1)
        if j < 0:
            raise ValueError("an unterminated quote in the program name")
        args, i = [s[i + 1:j]], j + 1
        if i < n and s[i] not in " \t":
            raise ValueError("text after the quoted program name")
    else:
        j = i
        while j < n and s[j] not in " \t":
            j += 1
        args, i = [s[i:j]], j
    while True:
        while i < n and s[i] in " \t":
            i += 1
        if i >= n:
            return args
        cur, inq = [], False
        while i < n:
            c = s[i]
            if c == "\\":
                j = i
                while j < n and s[j] == "\\":
                    j += 1
                if j < n and s[j] == '"':
                    cur.append("\\" * ((j - i) // 2))
                    if (j - i) % 2:
                        cur.append('"')
                        j += 1
                else:
                    cur.append("\\" * (j - i))
                i = j
                continue
            if c == '"':
                if inq and i + 1 < n and s[i + 1] == '"':
                    raise ValueError('`""` inside quotes (C runtimes disagree on it)')
                inq = not inq
                i += 1
                continue
            if c in " \t" and not inq:
                break
            cur.append(c)
            i += 1
        if inq:
            raise ValueError("an unterminated quote")
        args.append("".join(cur))


def split_command(cmd, nt=None):
    """A compile-database command as the build's process creation splits it (see crt_split); ValueError out of scope."""
    if os.name == "nt" if nt is None else nt:
        args = crt_split(cmd)
        if subprocess.list2cmdline(args[1:]) and crt_split("x " + subprocess.list2cmdline(args[1:]))[1:] != args[1:]:
            raise ValueError("the arguments do not survive re-quoting")
        return args
    if re.search(r"[$`;|&<>*?\[\]{}()~!#\n]", cmd):
        raise ValueError("a shell metacharacter in the command")
    return shlex.split(cmd)


def command_args(entry, split=split_command):
    """The argument list of a compile-database entry, without a cache launcher; ValueError when it cannot be split."""
    cmd = list(entry["arguments"]) if entry.get("arguments") else split(entry.get("command", ""))
    if cmd and os.path.basename(cmd[0]).lower() in LAUNCHERS:
        cmd = cmd[1:]
    return cmd


def driver_name(arg0):
    return os.path.basename(canon(arg0)).lower() if arg0 else ""


def classify_entry(e, split=split_command, name=driver_name):
    """('unit', args) | ('other', None) | ('out', why) for one compile-database entry. split / name: the command
    splitter and the driver-name resolver (the self-test passes file-system-free ones)."""
    f = e.get("file", "")
    is_tu = f.lower().endswith(TU_EXT)
    try:
        args = command_args(e, split)
    except ValueError as ex:
        rough = (e.get("command") or "").split()
        compiler = any(GNU_DRIVER.match(name(t.strip('"'))) or MSVC_DRIVER.match(name(t.strip('"')))
                       for t in rough[:2])
        if compiler or is_tu:
            return "out", "the command cannot be split reliably (%s)" % ex
        return "other", None
    drv = name(args[0]) if args else ""
    if MSVC_DRIVER.match(drv):
        return "out", "an MSVC-style compiler (%s)" % drv
    if GNU_DRIVER.match(drv):
        if "-c" in args:
            why = scope_problem(e, args)
            return ("out", why) if why else ("unit", args)
        if is_tu:
            return "out", "a C/C++ source built without -c"
        return "other", None
    if is_tu:
        return "out", "a C/C++ source built by a command that does not start with a compiler (%s)" % (drv or "empty")
    return "other", None


def forward_problem(args):
    """Why a command passes an option around the driver's own option parsing, or None. A forwarded option (-Wp,...,
    -Xpreprocessor, -Xclang, /clang:, a config file, a driver mode) can change preprocessing or the dependency output
    without the visible flags showing it (`-MD -Wp,-MMD,x.d` omits system headers). Accepted: -Xclang followed by
    -emit-pch or --dependent-lib=..., and -Xclang -include / -include-pch -Xclang <path>; nothing else."""
    k = 0
    while k < len(args):
        c = args[k]
        if c == "-Xclang":
            v = args[k + 1] if k + 1 < len(args) else ""
            if v in XCLANG_PATH:
                if k + 3 < len(args) and args[k + 2] == "-Xclang":
                    k += 4
                    continue
                return "-Xclang %s without -Xclang <path>" % v
            if v in XCLANG_OK or v.startswith(XCLANG_OK_PREFIX):
                k += 2
                continue
            return "a forwarded cc1 option (-Xclang %s)" % v
        if c.startswith(FORWARD_PREFIX):
            return "a forwarded / driver-level option (%s)" % c
        if c in DEPS_ALIASES:
            return "a long-form dependency option (%s)" % c
        k += 1
    return None


def scope_problem(e, args):
    """Why a compile edge (GNU driver with -c) is outside the scope, or None."""
    f = e.get("file", "")
    if not f.lower().endswith(TU_EXT):
        return "source %s is not one of %s" % (f, " ".join(TU_EXT))
    if args.count("-c") != 1 or args.count("-o") != 1:
        return "not exactly one -c and one -o"
    if any(c.startswith("-o") and c != "-o" for c in args):
        return "an attached -o<file>"
    if any(c.startswith("@") for c in args):
        return "a response file (@...) hides part of the command"
    fwd = forward_problem(args)
    if fwd:
        return fwd
    shaping = [c for c in args if c in PP_OUTPUT_FLAGS]
    if shaping:
        return "an option that shapes the -E output (%s): line markers / the token pass would not show the unit" % (
            " ".join(shaping))
    pchs = pch_options(args)
    if len(pchs) > 1 or "" in pchs:
        return "%d -include-pch option(s) (%s): the PCH clang uses is ambiguous" % (len(pchs), " ".join(map(repr, pchs)))
    mod = [c for c in args if c.startswith(MODULE_FLAGS)]
    if mod:
        return "module / PCH-preprocess flags (%s)" % " ".join(mod[:3])
    for k, c in enumerate(args):
        lang = args[k + 1] if c == "-x" and k + 1 < len(args) else c[2:] if c.startswith("-x") and len(c) > 2 else None
        if lang is not None and lang not in LANGS:
            return "language -x %s" % lang
    return None


def deps_mode_full(args):
    """The command's own deps record lists system headers: -MD and no -MMD / -MM."""
    return "-MD" in args and not any(c in ("-MMD", "-MM") or c in DEPS_ALIASES for c in args)


@lru_cache(maxsize=None)
def is_clang(driver):
    p = run([driver, "--version"])
    return p.returncode == 0 and "clang version" in p.stdout


def cc1_argv(line):
    """The argv of one `-###` job line: every argument double-quoted, with \\ escapes (clang escapes \\ " $ and `).
    ValueError if the line is anything else."""
    toks, pos = [], 0
    for m in re.finditer(r'[ \t]*"((?:\\.|[^"\\])*)"', line):
        if m.start() != pos:
            raise ValueError("unquoted text in a -### job at column %d" % pos)
        toks.append(re.sub(r"\\(.)", r"\1", m.group(1)))
        pos = m.end()
    if line[pos:].strip() or not toks:
        raise ValueError("unquoted text in a -### job at column %d" % pos)
    return toks


def deps_mode_of_job(argv):
    """('full' | 'partial' | 'unknown', why) of one cc1 argv's dependency output, read positionally - the layout the
    driver writes: -dependency-file F, then one or more -MT T, then -sys-header-deps if (and only if) system headers
    are listed. A value can never be mistaken for an option this way: F and every T are consumed as values, the flag
    must follow the last target directly, and any other occurrence of these tokens in the job (an `-I
    -sys-header-deps`, an `-MT -sys-header-deps` outside the group, a second group) makes it 'unknown'."""
    idx = [k for k, t in enumerate(argv) if t in CC1_DEPS]
    starts = [k for k in idx if argv[k] == "-dependency-file"]
    if len(starts) != 1 or starts[0] + 1 >= len(argv):
        return "unknown", "%d -dependency-file option(s) in the cc1 job" % len(starts)
    span, k, targets = {starts[0], starts[0] + 1}, starts[0] + 2, 0
    while k + 1 < len(argv) and argv[k] in ("-MT", "-MQ"):
        span |= {k, k + 1}
        k, targets = k + 2, targets + 1
    if not targets:
        return "unknown", "no -MT target after -dependency-file"
    full = k < len(argv) and argv[k] == "-sys-header-deps"
    if full:
        span.add(k)
    stray = ["%s@%d" % (argv[j], j) for j in idx if j not in span]
    if stray:
        return "unknown", "dependency tokens outside the driver's group (an option value?): %s" % " ".join(stray[:3])
    if not full:
        return "partial", "no -sys-header-deps after the -MT targets (the dependency output omits system headers)"
    return "full", None


def classify_hash_output(text, pch=None, cwd="", cn=None):
    """('full' | 'partial' | 'unknown', why) of a clang `-###` output: 'unknown' when a configuration file is in use
    (its options are not in the command - clang loads clang.cfg / <triple>-clang++.cfg ... on its own), when there is
    not exactly one cc1 job, when the job's precompiled header is not exactly the command's `pch` (None: no PCH; a
    relative path is taken from cwd) - several -include-pch, an empty one, or one the driver added for an -include x.h
    next to an x.h.pch - or when the job's dependency options cannot be classified."""
    cn = cn or canon
    cfg = [l.strip() for l in text.splitlines() if l.strip().startswith("Configuration file:")]
    if cfg:
        return "unknown", "a clang configuration file is in use (%s)" % cfg[0]
    jobs = [l for l in text.splitlines() if '"-cc1"' in l]
    if len(jobs) != 1:
        return "unknown", "%d cc1 job(s) in -###" % len(jobs)
    try:
        argv = cc1_argv(jobs[0])
    except ValueError as ex:
        return "unknown", str(ex)
    used = [argv[k + 1] if k + 1 < len(argv) else "" for k, t in enumerate(argv) if t == "-include-pch"]
    if any(t.startswith("-include-pch=") for t in argv) or len(used) > 1 or "" in used:
        return "unknown", "the cc1 job's precompiled header is ambiguous (%s)" % " ".join(map(repr, used))
    want = cn(os.path.join(cwd, pch)) if pch else None
    got = cn(os.path.join(cwd, used[0])) if used else None
    if got != want:
        return "unknown", "the cc1 job uses the precompiled header %s, the command shows %s" % (got, want)
    return deps_mode_of_job(argv)


def deps_effective(entry, args):
    """('full' | 'partial' | 'unknown', why): what the driver itself says this command's dependency output is
    (classify_hash_output of clang's `-###`). A config file, an alias or a forwarded flag cannot turn system headers
    off behind a visible -MD unseen. Only clang can show it: another driver is 'partial' (its records are not used)."""
    if not args or not is_clang(args[0]):
        return "partial", "not clang: the effective deps mode cannot be shown"
    p = run([args[0], "-###"] + list(args[1:]), cwd=entry["directory"])
    if p.returncode:
        return "unknown", "`-###` exited %d" % p.returncode
    return classify_hash_output(p.stderr, pch_of(args), entry["directory"])


def pch_options(args):
    """Every -include-pch value of a command (-include-pch X, -include-pch=X, or through -Xclang)."""
    plain = [a for a in args if a != "-Xclang"]
    vals = []
    for k, c in enumerate(plain):
        if c == "-include-pch":
            vals.append(plain[k + 1] if k + 1 < len(plain) else "")
        elif c.startswith("-include-pch="):
            vals.append(c.split("=", 1)[1])
    return vals


def pch_of(args):
    """The precompiled header a command uses, or None. scope_problem() admits at most one, non-empty, -include-pch
    (clang uses the last of several; `-include-pch x.pch -include-pch ""` loads none), and deps_effective() checks
    that clang's cc1 job uses exactly that one (an -include x.h next to an x.h.pch is a PCH too)."""
    vals = pch_options(args)
    return vals[0] if vals else None


def strip_outputs(cmd):
    """The compile command without -c, its object output and its dependency-file flags."""
    res, skip = [], 0
    for c in cmd:
        if skip:
            skip -= 1
            continue
        if c in ("-o", "-MF", "-MT", "-MQ"):
            skip = 1
            continue
        if c in ("-c", "-MD", "-MMD", "-MM", "-MG", "-MP") or c.startswith(("-MF", "-MT", "-MQ")):
            continue
        res.append(c)
    return res


def preprocess(entry, out_path, args=None):
    """(ok, error or None, read set or None) of one unit preprocessed to out_path; its dependency list (-MD) goes to
    out_path + '.d' and becomes the read set."""
    cmd = strip_outputs(args or command_args(entry)) + ["-E", "-P", "-MD", "-MF", out_path + ".d", "-o", out_path]
    p = run(cmd, cwd=entry["directory"])
    if p.returncode:
        return False, "the preprocessor exited %d: %s" % (p.returncode, p.stderr.strip()[-600:]), None
    try:
        md = open(out_path + ".d", encoding="utf-8", errors="surrogateescape").read()
    except OSError as ex:
        return False, "no dependency list: %s" % ex, None
    ok, files = read_set_from_make(md, entry)
    if not ok:
        return False, "the -E dependency list: %s" % files, None
    return True, None, {canon(x) for x in files}


def marker_command(args, out_path):
    """The -E command of the marker pass: the unit's command without its outputs and without any option that shapes
    the -E output (-P would drop every marker), plus clang's cc1 -dE (`-Xclang -dE`; the driver's -dE is silently
    dropped): every #embed is then printed as a canonical `#embed` line instead of being expanded, so clang's own
    lexer finds embeds whatever their spelling."""
    return [c for c in strip_outputs(args) if c not in PP_OUTPUT_FLAGS] + ["-E", "-Xclang", "-dE", "-o", out_path]


EMBED_OUT = re.compile(r"^[ \t]*#[ \t]*embed\b")  # an #embed directive as clang -dE prints it


def embed_lines(out_path):
    """The #embed directives a marker pass printed (marker_command's -dE), or None if its output cannot be read."""
    try:
        with open(out_path, encoding="utf-8", errors="surrogateescape") as f:
            return [l.rstrip() for l in f if EMBED_OUT.match(l)]
    except OSError:
        return None


@lru_cache(maxsize=None)
def shows_embeds(driver):
    """Does `driver -E -Xclang -dE` print an #embed directive (canonical, unexpanded)? Probed once per driver in a
    temp folder; without it no unit of that driver can be cleared of #embed."""
    d = tempfile.mkdtemp(prefix="pp_receipts_dE_")
    try:
        open(os.path.join(d, "blob.bin"), "wb").write(b"\x41\x42")
        open(os.path.join(d, "probe.cpp"), "w").write('const unsigned char b[] = {\n#/**/embed "blob.bin"\n};\n')
        p = run([driver, "-E", "-Xclang", "-dE", "probe.cpp"], cwd=d)
        lines = p.stdout.splitlines()
        return p.returncode == 0 and any(EMBED_OUT.match(l) for l in lines) and not any("65" in l for l in lines)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def markers_entered(text, entry, cn=None):
    """The files the line markers of a plain -E output name, or None unless they positively name the unit's own source
    (zero markers - a suppressed pass - is no evidence, never 'nothing entered')."""
    cn = cn or canon
    files = set()
    for line in text.splitlines():
        if line.startswith("#"):
            m = re.match(r'#[ \t]*(?:line[ \t]+)?\d+[ \t]+"((?:\\.|[^"\\])*)"', line)
            if m:
                files.add(cn(os.path.join(entry["directory"], re.sub(r"\\(.)", r"\1", m.group(1)))))
    return files if cn(os.path.join(entry["directory"], entry["file"])) in files else None


def entered_files(entry, args, out_path):
    """The files a unit's preprocessing ENTERS - the line markers of a plain -E pass (clang writes one on entering every
    file, an empty one too), written to out_path - or None if that pass fails or its markers do not name the unit's
    own source. A file the dependency output lists and no marker names was recorded without being entered: probed by
    __has_include, or skipped."""
    p = run(marker_command(args, out_path), cwd=entry["directory"])
    if p.returncode or not os.path.exists(out_path):
        return None
    with open(out_path, encoding="utf-8", errors="surrogateescape") as f:
        return markers_entered(f.read(), entry)


def parse_deps(text):
    """{target: (valid, [deps])} of `ninja -t deps`; ValueError on anything it does not understand."""
    hdr = re.compile(r"^(.*): #deps (\d+), deps mtime (-?\d+) \((VALID|STALE)\)$")
    recs, cur = {}, None
    for line in text.splitlines():
        if not line.strip():
            cur = None
            continue
        if line[0] in " \t":
            if cur is None:
                raise ValueError("a dependency line outside a record: %r" % line)
            recs[cur][2].append(line.strip())
            continue
        m = hdr.match(line)
        if not m:
            raise ValueError("unparsable `ninja -t deps` line: %r" % line)
        cur = m.group(1)
        if cur in recs:
            raise ValueError("two records for %s" % cur)
        recs[cur] = [m.group(4) == "VALID", int(m.group(2)), []]
    for t, (valid, count, deps) in recs.items():
        if len(deps) != count:
            raise ValueError("the record of %s lists %d of its %d deps" % (t, len(deps), count))
    return {t: (r[0], r[2]) for t, r in recs.items()}


def parse_make_deps(text, cwd):
    """The files of a make-style dependency list (clang / gcc -M)."""
    body = re.sub(r"\\\r?\n", " ", text)
    k = body.find(": ")
    if k < 0:
        k = body.rstrip().find(":")
        if k < 0 or body.rstrip()[k + 1:].strip():
            raise ValueError("no `target: deps` in the -M output")
        return []
    files = [t.replace("\\ ", " ").replace("$$", "$") for t in re.findall(r"(?:\\ |[^\s])+", body[k + 2:])]
    return [f if os.path.isabs(f) else os.path.join(cwd, f) for f in files]


def read_set_from_make(md, entry, cn=None):
    """(ok, files or error) of an -M output: substantive evidence only - it must name the unit's source."""
    cn = cn or canon
    try:
        files = parse_make_deps(md, entry["directory"])
    except ValueError as ex:
        return False, str(ex)
    if cn(os.path.join(entry["directory"], entry["file"])) not in {cn(f) for f in files}:
        return False, "the -M output does not name the unit's source (%d files)" % len(files)
    return True, files


def compiler_deps(entry, args=None):
    """(ok, files or error) - the unit's read set from the compiler itself (`-M`: every header, system ones too)."""
    cmd = strip_outputs(args or command_args(entry)) + ["-M"]
    if cmd and is_clang(cmd[0]):
        mode, why = deps_effective(entry, cmd)
        if mode != "full":
            return False, "the -M command (%s): %s" % (mode, why)
    p = run(cmd, cwd=entry["directory"])
    if p.returncode:
        return False, "the compiler's -M exited %d: %s" % (p.returncode, p.stderr.strip()[-300:])
    return read_set_from_make(p.stdout, entry)


def has_include_recorded(args0):
    """(ok, why) - does this compiler list a file found only by __has_include in its -M output? (A new or deleted file
    probed by __has_include changes a unit that never opens it; the read sets see it only if the compiler
    records it.)"""
    d = tempfile.mkdtemp(prefix="pp_receipts_probe_")
    try:
        open(os.path.join(d, "probe.cpp"), "w").write('#if __has_include("probe_only.h")\nint x;\n#endif\n')
        open(os.path.join(d, "probe_only.h"), "w").write("\n")
        p = run([args0, "-x", "c++", "-M", "probe.cpp"], cwd=d)
        if p.returncode:
            return False, "the probe exited %d: %s" % (p.returncode, p.stderr.strip()[-200:])
        return ("probe_only.h" in p.stdout), "its -M output does not list a __has_include'd file"
    finally:
        shutil.rmtree(d, ignore_errors=True)


def expand_vars(line, env):
    """Top-level `$name` / `${name}` of a build.ninja line expanded (escapes kept for the tokenizer)."""
    out, k, n = [], 0, len(line)
    while k < n:
        c = line[k]
        if c != "$" or k + 1 >= n:
            out.append(c)
            k += 1
            continue
        nx = line[k + 1]
        if nx == "{":
            j = line.find("}", k)
            if j < 0:
                raise ValueError("an unterminated ${ in %s" % line[:200])
            out.append(env.get(line[k + 2:j], ""))
            k = j + 1
        elif re.match(r"[A-Za-z0-9_-]", nx):
            m = re.match(r"[A-Za-z0-9_-]+", line[k + 1:])
            out.append(env.get(m.group(0), ""))
            k += 1 + m.end()
        else:
            out.append(line[k:k + 2])
            k += 2
    return "".join(out)


def ninja_edges(text, bdir, read_include=None, env=None):
    """[(outputs, rule, inputs)] of a build.ninja (paths with top-level variables expanded, `include` / `subninja`
    followed), for the consumer check. ValueError on a line it cannot read."""
    lines, buf = [], ""
    for raw in text.splitlines():
        if raw.endswith("$") and not raw.endswith("$$"):
            buf += raw[:-1]
            continue
        lines.append(buf + raw.lstrip() if buf else raw)
        buf = ""
    env = {} if env is None else env
    edges = []
    for line in lines:
        m = re.match(r"^([A-Za-z0-9_.-]+)\s*=\s*(.*)$", line)
        if m:
            env[m.group(1)] = expand_vars(m.group(2), env)
            continue
        if line.startswith(("include ", "subninja ")):
            if read_include is None:
                raise ValueError("an include in a build file the caller did not resolve: %s" % line)
            edges += ninja_edges(read_include(line.split(None, 1)[1].strip()), bdir, read_include, env)
            continue
        if not line.startswith("build "):
            continue
        body = expand_vars(line[6:], env)
        toks, cur, k = [], "", 0
        while k < len(body):
            c = body[k]
            if c == "$" and k + 1 < len(body):
                nx = body[k + 1]
                cur += nx if nx in " :$\n" else c + nx
                k += 2
                continue
            if c == " ":
                if cur:
                    toks.append(cur)
                cur = ""
            elif c == ":" and ":" not in toks and (k + 1 == len(body) or body[k + 1] == " "):
                if cur:
                    toks.append(cur)
                toks.append(":")
                cur = ""
            else:
                cur += c
            k += 1
        if cur:
            toks.append(cur)
        if ":" not in toks or toks.index(":") + 1 >= len(toks):
            raise ValueError("a build line without `: rule`: %s" % line[:200])
        c = toks.index(":")
        outs = [t for t in toks[:c] if t not in ("|", "|@")]
        ins = [t for t in toks[c + 2:] if t not in ("|", "||", "|@")]
        edges.append((outs, toks[c + 1], ins))
    return edges


@lru_cache(maxsize=None)
def stat_ns(p):
    try:
        return os.stat(p).st_mtime_ns
    except OSError:
        return None


def compile_units(compdb, cn, classify=classify_entry):
    """([{obj, src, entry, args}], problems): every compile edge of the database, each checked against the scope."""
    units, problems, seen = [], [], set()
    for e in compdb:
        what, x = classify(e)
        if what == "other":
            continue
        out = e.get("output")
        if what == "out" or not out:
            problems.append("compile edge for %s: %s - out of scope" % (out or e.get("file"), x or "no output"))
            continue
        obj = cn(os.path.join(e["directory"], out))
        if obj in seen:
            problems.append("two compile edges write %s - out of scope" % obj)
            continue
        seen.add(obj)
        units.append({"obj": obj, "src": cn(os.path.join(e["directory"], e["file"])), "entry": e, "args": x})
    return units, problems


def read_sets_from_compiler(units, fallback, jobs, cn):
    """{obj: set} and the problems, every unit through `fallback` (the compiler's -M)."""
    res, problems = {}, []
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        for u, (ok, r) in zip(units, ex.map(lambda u: fallback(u["entry"], u["args"]), units)):
            if ok:
                res[u["obj"]] = {cn(x) for x in r}
            else:
                problems.append("no BEFORE read set for %s: %s" % (u["obj"], r))
    return res, problems


def resolve_assumed(patterns, files):
    """{repo path: reason} of --assume-no-codegen PAT=REASON over the changed files, and the problems."""
    res, problems = {}, []
    for p in patterns:
        pat, sep, why = p.partition("=")
        if not sep or not pat.strip() or not why.strip():
            problems.append("--assume-no-codegen %r: PATTERN=REASON, the reason is required" % p)
            continue
        hit = [f for f in files if fnmatch.fnmatchcase(f, pat.strip())]
        if not hit:
            problems.append("--assume-no-codegen %r matches no changed file" % pat)
        for f in hit:
            res[f] = why.strip()
    return res, problems


def consumers_of(edges, recs, objs, bdir, cn):
    """{file: [non-compile outputs that consume it]}: declared inputs of every edge that is not a compile unit or a
    phony alias (followed through phony aliases), plus the deps records of non-compile targets."""
    ab = lambda p: cn(p if os.path.isabs(p) else os.path.join(bdir, p))  # noqa: E731
    phony = {}
    for outs, rule, ins in edges:
        if rule == "phony":
            for o in outs:
                phony.setdefault(ab(o), []).extend(ab(i) for i in ins)
    res = {}

    def add(p, out, seen):
        res.setdefault(p, []).append(out)
        for q in phony.get(p, ()):
            if q not in seen:
                seen.add(q)
                add(q, out, seen)

    for outs, rule, ins in edges:
        if rule == "phony" or any(ab(o) in objs for o in outs):
            continue
        for i in ins:
            add(ab(i), outs[0] if outs else "?", {ab(i)})
    for t, (valid, deps) in recs.items():
        if ab(t) in objs:
            continue
        for d in deps:
            res.setdefault(ab(d), []).append(t + " (its deps record)")
    return res


def discover(bdir, root, kind, assumed, deps_out, compdb_out, edges=(), stat=stat_ns, fallback=compiler_deps, jobs=8,
             before_reads=None, cn=canon, classify=classify_entry, effective=deps_effective):
    """(affected units, coverage lines, problems) of a build dir. kind = {repo path: 'changed' | 'new' | 'deleted'};
    assumed = {repo path: reason}; deps_out / compdb_out = (exit code, stdout) of `ninja -t deps` / `-t compdb`;
    edges = ninja_edges() of build.ninja; before_reads = {obj: BEFORE read set} (required with a deleted file);
    effective(entry, args) = ('full' | 'partial' | 'unknown', why) of the command's dependency output
    (deps_effective): full = the record may be used, partial = compiler -M instead, unknown = INCOMPLETE. Any problem =
    INCOMPLETE."""
    problems = []
    if deps_out[0] != 0:
        problems.append("`ninja -t deps` exited %d" % deps_out[0])
    if compdb_out[0] != 0:
        problems.append("`ninja -t compdb` exited %d" % compdb_out[0])
    if problems:
        return [], [], problems
    try:
        recs = parse_deps(deps_out[1])
    except ValueError as ex:
        return [], [], ["ninja deps: %s" % ex]
    try:
        units, problems = compile_units(json.loads(compdb_out[1]), cn, classify)
    except ValueError as ex:
        return [], [], ["ninja compdb: %s" % ex]
    if not units:
        return [], [], problems + ["the compile database has no compile edge in scope"]
    by_obj = {cn(os.path.join(bdir, t)): r for t, r in recs.items()}
    need, cand = [], []
    for u in units:
        rec, why = by_obj.get(u["obj"]), None
        if not deps_mode_full(u["args"]):
            why = "the command's deps mode is not -MD (system headers may be missing)"
        elif rec is None:
            why = "no deps record"
        elif not rec[0]:
            why = "deps record STALE"
        elif not rec[1]:
            why = "deps record empty"
        else:
            files = [os.path.join(bdir, d) if not os.path.isabs(d) else d for d in rec[1]]
            nf = {cn(x) for x in files}
            o = stat(u["obj"])
            if u["src"] not in nf:
                why = "deps record does not name the source"
            elif o is None:
                why = "object missing"
            elif any(stat(x) is None or stat(x) > o for x in files):
                why = "a dependency is newer than the object"
            else:
                cand.append((u, nf))
        if why:
            u["how"] = "compiler -M (%s)" % why
            need.append(u)
    via_deps = 0
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        for (u, nf), (mode, w) in zip(cand, ex.map(lambda c: effective(c[0]["entry"], c[0]["args"]), cand)):
            if mode == "full":
                u["reads"], u["how"] = nf, "ninja deps"
                via_deps += 1
            elif mode == "partial":
                u["how"] = "compiler -M (the driver does not show full deps: %s)" % w
                need.append(u)
            else:
                problems.append("the effective deps mode of %s cannot be established: %s - out of scope" %
                                (u["obj"], w))
    failed = 0
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        for u, (ok, res) in zip(need, ex.map(lambda u: fallback(u["entry"], u["args"]), need)):
            if ok:
                u["reads"] = {cn(x) for x in res}
            else:
                failed += 1
                problems.append("no read set for %s (%s): %s" % (u["obj"], u["how"], res))
    reasons = {}
    for u in need:
        reasons[u["how"]] = reasons.get(u["how"], 0) + 1
    cov = ["units (compile edges, all in scope) scanned: %d" % len(units),
           "AFTER read sets from ninja deps (-MD records whose full mode clang -### shows): %d" % via_deps,
           "AFTER read sets from the compiler (-M): %d%s" % (len(need) - failed, "".join(
               "; %d %s" % (v, k) for k, v in sorted(reasons.items()))),
           "units without an AFTER read set: %d" % failed]
    deleted = [f for f, k in kind.items() if k == "deleted"]
    if deleted:
        if before_reads is None:
            problems.append("files deleted since base but no BEFORE read sets: %s" % " ".join(deleted))
            before_reads = {}
        cov.append("BEFORE read sets (compiler -M, BEFORE files in place, for the deleted files): %d" %
                   len(before_reads))
    objs = {u["obj"] for u in units}
    consumers = consumers_of(edges, recs, objs, bdir, cn)
    pch_users = {}
    for u in units:
        pch = pch_of(u["args"])
        if pch:
            pch_users.setdefault(cn(pch if os.path.isabs(pch) else os.path.join(u["entry"]["directory"], pch)),
                                 []).append(u)
    by_unit_obj = {u["obj"]: u for u in units}
    changed_paths = {cn(os.path.join(root, f)): f for f, k in kind.items()}
    for pch, us in sorted(pch_users.items()):
        pu = by_unit_obj.get(pch)
        if pu is None:
            problems.append("%d unit(s) use the precompiled header %s, which no compile edge in scope builds" %
                            (len(us), pch))
            continue
        inside = sorted(changed_paths[p] for p in pu.get("reads", ()) if p in changed_paths)
        if inside:
            problems.append("changed file(s) inside the precompiled header %s (%s), used by %d unit(s) - out of scope"
                            % (pch, " ".join(inside), len(us)))
        for u in us:  # the PCH build's own record, for read_set_mismatch: only a trusted, full -MD record counts
            u["pch_reads"] = pu["reads"] if pu.get("how") == "ninja deps" else None
    readers = {}
    for f, k in sorted(kind.items()):
        p = cn(os.path.join(root, f))
        if k == "deleted":
            rs = [u for u in units if p in (before_reads or {}).get(u["obj"], ())]
        else:
            rs = [u for u in units if p in u.get("reads", ())]
        readers[f] = rs
        gen = consumers.get(p, [])
        if gen:
            problems.append("%s is consumed by a non-compile build edge (%s) - a generator / custom command / resource "
                            "or configure input: out of scope" % (f, " ".join(sorted(set(gen))[:3])))
        if rs and f in assumed:
            problems.append("--assume-no-codegen names %s, but %d unit(s) read it" % (f, len(rs)))
        if not rs and f not in assumed:
            problems.append("no unit of this build dir reads %s (%s): its effect on the build is not modelled - pass "
                            "--assume-no-codegen '%s=<reason>' only if it generates no compiled input" % (f, k, f))
        cov.append("changed %s (%s): %s" % (f, k, ("%d reader(s)" % len(rs)) if rs else
                                            "no reader; --assume-no-codegen: " + assumed.get(f, "(none)")))
    hit = {u["obj"] for rs in readers.values() for u in rs}
    affected = [u for u in units if u["obj"] in hit]
    if not affected:
        problems.append("no affected unit: nothing would be compared")
    cov.append("affected units: %d" % len(affected))
    return affected, cov, problems


SYS_INCLUDE_OPTS = ("-internal-isystem", "-isystem", "-internal-externc-isystem")  # cc1 system include directories
EMBED = re.compile(r"^[ \t]*(?:#|%:)[ \t]*embed\b", re.M)


def unit_witness(entry, args, entered):
    """What the PCH-probe excuse needs to know about a unit's own preprocessing, or None: {'entered': the files it
    enters (entered_files: line markers naming its source), 'sysdirs': the system include directories of its effective
    cc1 job (-internal-isystem / -isystem / -internal-externc-isystem as clang's -### reports them), 'resource': the
    cc1 job's -resource-dir (where clang's builtin headers live)}. None without the markers or a clang cc1 job."""
    if entered is None or not args:
        return None
    p = run([args[0], "-###"] + list(args[1:]), cwd=entry["directory"])
    jobs = [l for l in p.stderr.splitlines() if '"-cc1"' in l]
    if p.returncode or len(jobs) != 1:
        return None
    try:
        argv = cc1_argv(jobs[0])
    except ValueError:
        return None
    at = lambda k: canon(os.path.join(entry["directory"], argv[k + 1])) if k + 1 < len(argv) else None  # noqa
    sysdirs = [at(k) for k, t in enumerate(argv) if t in SYS_INCLUDE_OPTS and at(k)]
    res = [at(k) for k, t in enumerate(argv) if t == "-resource-dir" and at(k)]
    return {"entered": entered, "sysdirs": sysdirs, "resource": res[0] if len(res) == 1 else None}


def read_text_cached(path, cache={}):  # noqa: B006 - a per-run cache of header texts
    if path not in cache:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                cache[path] = f.read()
        except OSError:
            cache[path] = None
    return cache[path]


def embed_in(files, read=read_text_cached):
    """The first of these (entered) files that holds an #embed directive (line splices removed; # or %:, any
    whitespace), or None. An embedded file is read without a line marker, so no unit that embeds is excused."""
    for f in sorted(files):
        text = read(f)
        if text and EMBED.search(text.replace("\\\r\n", "").replace("\\\n", "")):
            return f
    return None


def marker_problem(entered, embeds, read=read_text_cached):
    """Why a unit cannot be receipted on its marker-pass evidence, or None. An embedded input is read without a line
    marker, so a unit that embeds is out of scope. clang decides: `embeds` = the #embed lines its -dE marker pass
    printed (embed_lines), None when there is no such answer (the driver cannot show embeds, or the output is
    unreadable) - both FAILED. A text scan of the entered files (embed_in) is an extra check only."""
    if entered is None:
        return ("its line-marker pass gave no answer (no marker names its source), so #embed and the PCH-probe excuse "
                "cannot be checked")
    if embeds is None:
        return "clang gave no -dE answer for its #embed directives (shows_embeds / the marker pass output)"
    if embeds:
        return "an #embed directive (clang -dE: %s): an embedded input leaves no line marker - out of scope" % (
            embeds[0][:120])
    emb = embed_in(entered, read)
    return "an #embed directive in %s: an embedded input leaves no line marker - out of scope" % emb if emb else None


def probe_excuse(f, w, read=read_text_cached):
    """Why the file f - in the unit's AFTER read set, not in its build record, in its PCH's trusted build record - is
    NOT excused as a builtin header's probe, or None when it is: (a) no line marker of the unit names it; (b) it lies
    under a system include directory of the unit's cc1 job; (c) a header the unit DID enter, under the cc1 job's
    resource dir (a clang builtin header), contains `__has_include_next(<NAME>)` for its include name NAME (its path
    under that system directory). This is the one cause found in real use (clang's stddef.h / stdarg.h, line 22);
    anything else is not excused."""
    if f in w["entered"]:
        return "entered (a line marker names it)"
    under = lambda p, d: p.startswith(d.rstrip("/") + "/")  # noqa: E731
    names = sorted({f[len(d.rstrip("/")) + 1:] for d in w["sysdirs"] if under(f, d)})
    if not names:
        return "not under a system include directory of the cc1 job"
    res = w.get("resource")
    builtins = sorted(h for h in w["entered"] if res and under(h, res))
    flags = re.I if os.name == "nt" else 0
    for h in builtins:
        text = read(h) or ""
        for n in names:
            if re.search(r"__has_include_next\s*\(\s*<\s*%s\s*>\s*\)" % re.escape(n), text, flags):
                return None
    return "no entered clang builtin header (under %s) probes <%s> with __has_include_next" % (res, names[0])


def read_set_mismatch(u, seen, witness=None, read=read_text_cached):
    """Why the files a unit's AFTER preprocessing read differ from its build's deps record, or None.

    One difference is explained, and only this one: a unit that uses a precompiled header. Its -c record lists the
    files it reads itself plus the PCH's input files, and a PCH's input files are the files ENTERED while it was
    built. clang's builtin stddef.h / stdarg.h probe the next header of their name (`__has_include_next(<stddef.h>)`,
    line 22) without entering it - the UCRT stddef.h, the MSVC stdarg.h. Such a file is in the PCH unit's deps
    record but not in the PCH, so not in the consumer's record; preprocessing alone (-E, -M) runs the PCH's header
    again and records the probe. A file in the AFTER read set and not in the unit's record is excused only if ALL of:
    its PCH's trusted build record (a current, full -MD record) lists it, and probe_excuse() finds it unentered, under
    a system include directory, and named by an entered builtin header's __has_include_next. witness() (called only
    then) is unit_witness(); no answer, or any file failing one condition, is a mismatch. Anything else - a file in
    neither record, a file the record lists that the preprocessing did not read, a PCH whose read set did not come
    from its build record - stays a mismatch. (A changed file inside the PCH's read set, probed ones included, is
    already INCOMPLETE in discover; a unit that embeds (#embed) is FAILED before this.)"""
    u.pop("pch_explained", None)
    if u.get("how") != "ninja deps" or seen is None:
        return None
    if seen == u["reads"]:
        return None
    extra, missing = seen - u["reads"], u["reads"] - seen
    probed = extra & (u.get("pch_reads") or set())
    unexplained = sorted(extra - probed)
    if probed and not unexplained and not missing:
        w = witness() if witness else None
        if not w or w.get("entered") is None:
            return ("its preprocessing read %d file(s) only its PCH's record lists, and its own preprocessing could "
                    "not be witnessed: %s" % (len(probed), " ".join(sorted(probed)[:2])))
        bad = {f: probe_excuse(f, w, read) for f in sorted(probed)}
        bad = {f: why for f, why in bad.items() if why}
        if not bad:
            u["pch_explained"] = sorted(probed)
            return None
        f = sorted(bad)[0]
        return ("its preprocessing read %d file(s) the build's record does not list and that are not a builtin "
                "header's probe (its PCH's record lists them, which proves nothing more): %s - %s" % (len(bad), f,
                                                                                                    bad[f]))
    missing = sorted(missing)
    return ("its preprocessing read a different file set from the build's deps record (%d not in the record and not "
            "in its PCH's record, e.g. %s; %d not read, e.g. %s)" % (len(unexplained), " ".join(unexplained[:2]),
                                                                      len(missing), " ".join(missing[:2])))


def receipt_names(objs, bdir):
    """{obj: a collision-free file name}; ValueError when two objects would share one."""
    names = {}
    for o in objs:
        rel = os.path.relpath(o, bdir).replace("\\", "/")
        base = re.sub(r"[^A-Za-z0-9._-]", "_", rel.rsplit("/", 1)[-1])[:80]
        names[o] = "%s_%s" % (base, hashlib.sha1(rel.encode("utf-8")).hexdigest()[:12])
    if len(set(n.lower() for n in names.values())) != len(names):
        raise ValueError("two objects map to one receipt name")
    return names


def env_problems(environ):
    return ["%s is set in the environment (it changes what the compiler reads or defines) - unset it" % v
            for v in DRIVER_ENV if environ.get(v)]


def path_problems(files):
    """Changed paths a case-insensitive file system would merge."""
    seen, problems = {}, []
    for f in files:
        k = f.lower()
        if k in seen:
            problems.append("%s and %s differ only in case - out of scope" % (seen[k], f))
        seen[k] = f
    return problems


def tree_problems(files, base):
    """Out-of-scope states of the working tree and the index."""
    problems = path_problems(files)
    top = run(GIT + ["rev-parse", "--show-toplevel"])
    if top.returncode or norm(top.stdout.strip()) != norm(os.getcwd()):
        problems.append("run from the top level of the repo (%s)" % top.stdout.strip())
    u = run(GIT + ["ls-files", "--others", "--exclude-standard", "-z"])
    if u.returncode:
        problems.append("`git ls-files --others` exited %d" % u.returncode)
    elif u.stdout.strip("\0"):
        problems.append("untracked, not ignored files (commit or remove them): %s" %
                        " ".join(u.stdout.split("\0")[:10]))
    v = run(GIT + ["ls-files", "-v", "-z"])
    flagged = [x[2:] for x in v.stdout.split("\0") if x[:1].islower() or x[:1] == "S"]
    if v.returncode or flagged:
        problems.append("assume-unchanged / skip-worktree entries hide working-tree changes: %s" %
                        " ".join(flagged[:10]))
    for ref in (base, "HEAD"):
        ls = run(GIT + ["ls-tree", "-r", "-z", ref, "--"] + files)
        for rec in ls.stdout.split("\0"):
            if rec[:6] in ("160000", "120000"):
                problems.append("%s is a %s at %s - out of scope" %
                                (rec.split("\t", 1)[-1], "submodule" if rec[:6] == "160000" else "symlink", ref))
    for f in files:
        if os.path.isdir(f) or os.path.islink(f):
            problems.append("%s is a directory / link in the tree - out of scope" % f)
    return problems


@contextmanager
def before_tree(kind, base):
    """The changed files as a checkout of <base> writes them (`git cat-file --filters`), NEW ones moved aside;
    restored with their mtimes afterwards."""
    keep = {}
    try:
        for f, k in kind.items():
            keep[f] = (open(f, "rb").read(), os.stat(f)) if os.path.exists(f) else None
            if k == "new":
                os.remove(f)
                continue
            shown = subprocess.run(GIT + ["cat-file", "--filters", "%s:%s" % (base, f)], capture_output=True)
            if shown.returncode:
                raise RuntimeError("`git cat-file --filters %s:%s` exited %d" % (base, f, shown.returncode))
            os.makedirs(os.path.dirname(f) or ".", exist_ok=True)
            open(f, "wb").write(shown.stdout)
        yield
    finally:
        for f, v in keep.items():
            if v is None:
                if os.path.exists(f):
                    os.remove(f)
            else:
                open(f, "wb").write(v[0])
                os.utime(f, ns=(v[1].st_atime_ns, v[1].st_mtime_ns))
        stat_ns.cache_clear()
        canon.cache_clear()


def self_test(cc=None):
    bad = []

    def check(name, cond):
        print(("ok   " if cond else "FAIL ") + name)
        if not cond:
            bad.append(name)

    def lex_fails(text):
        try:
            tokens(text)
            return False
        except LexError:
            return True

    def tk(text):
        """tokens(), with a LexError turned into a value no expected stream equals (a FAIL, not a crash)."""
        try:
            return tokens(text)
        except LexError:
            return ["<LexError>"]  # constant: two failures compare equal, so a `!=` check cannot pass on them

    def raises(f, *a):
        try:
            f(*a)
            return False
        except ValueError:
            return True

    print("-- the lexer (literals, punctuators, scope)")
    check("a raw literal with an embedded blank line differs from one without",
          tk('auto s = R"x(a\n\nb)x";\n') != tk('auto s = R"x(a\nb)x";\n'))
    check("... with a double quote in the delimiter too (R\"\"(...)\"\")",
          tk('R""(a\n\nb)""') != tk('R""(a\nb)""') and tk('R""(a\n\nb)""') == ['R""(a\n\nb)""'])
    check("a raw literal keeps its exact contents as one token",
          tk('R"(a\n\n b)"') == ['R"(a\n\n b)"'] and tk('u8R"d( x )d"') == ['u8R"d( x )d"'])
    check("a 16-character delimiter is legal, a 17-character one is rejected",
          tk('R"abcdefghijklmnop(x)abcdefghijklmnop"') != ["<LexError>"]
          and lex_fails('R"abcdefghijklmnopq(x)abcdefghijklmnopq"'))
    check("unrecognised literals are rejected (unterminated raw, string, char; stray quote)",
          lex_fails('R"x(abc') and lex_fails('"abc\n"') and lex_fails("'a\n") and lex_fails("a ' b"))
    check("whitespace and blank lines outside literals are not tokens",
          tk("int  a =\n\n\t 1 ;\n") == tk("int a = 1;"))
    check("spaces inside a string literal count", tk('f("a  b");') != tk('f("a b");'))
    check("u8 / L / u / U prefixed literals are one token each",
          tk('L"a" u8"b" u\'c\' U"d" L\'e\'') == ['L"a"', 'u8"b"', "u'c'", 'U"d"', "L'e'"])
    check("user-defined suffixes stay with their literal",
          tk('"a"_x \'c\'_y 12_km 1.5e+3_f R"(r)"_z') == ['"a"_x', "'c'_y", "12_km", "1.5e+3_f", 'R"(r)"_z']
          and tk('"a"_x') != tk('"a" _x'))
    check("digraphs are tokens, and <:: splits as < :: unless followed by : or >",
          tk("<% %> <: :> %: %:%:") == ["<%", "%>", "<:", ":>", "%:", "%:%:"]
          and tk("x<::y") == ["x", "<", "::", "y"] and tk("a<::>") == ["a", "<:", ":>"])
    check("an equal pair (formatting only) compares equal",
          tk("if (a &&\n    b) {\n  x(1e+5, 'q');\n}\n") == tk("if (a && b) { x(1e+5, 'q'); }"))
    check("a differing pair compares different", tk("x = a + b;") != tk("x = a - b;"))
    check("maximal munch: a - -b differs from a --b", tk("a - -b") != tk("a --b"))
    check("a #pragma line keeps its end of line", tk("#pragma once\nint a;") != tk("#pragma once int a;"))
    check("attack: a line marker in -P output is rejected (it could renumber or hide lines)",
          lex_fails('# 12 "x.cpp"\nint a;') and lex_fails('#line 7 "y.h"\n'))
    check("attack: a directive other than #pragma is rejected (#embed would hide the embedded file's bytes)",
          lex_fails('int d[] = {\n#embed "blob.bin"\n};') and lex_fails("%:embed <x>\n") and lex_fails("#\n")
          and not lex_fails("#pragma pack(1)\n%:pragma once\n"))
    check("attack: non-ASCII identifiers are rejected (a<U+03B2>; vs a <U+03B2>; would tokenize alike)",
          lex_fails("int a\u03b2;") and lex_fails("int a \u03b2;") and lex_fails("\u03b1\u03b2;"))
    check("... a byte-order mark outside a literal too, while non-ASCII inside a literal is kept",
          lex_fails("\ufeffint a;") and tk('f("\u03b1\u03b2");') != tk('f("\u03b1 \u03b2");'))
    def sx(raw):  # through read_text, as the receipts read the preprocessed files
        fd, tmp = tempfile.mkstemp(prefix="pp_receipts_bytes_")
        os.write(fd, raw)
        os.close(fd)
        try:
            return read_text(tmp)
        finally:
            os.remove(tmp)

    check("attack: two different undecodable bytes in a literal stay different (errors='replace' merged them)",
          tk(sx(b'f("\xff");')) != tk(sx(b'f("\xfe");')) and lex_fails(sx(b"int a\xff;")))

    print("-- no exception class: a difference is DIFF")
    a10, a999 = tk('f("a.cpp", 10);'), tk('f("a.cpp", 999);')
    check("an integer change next to a file name is a difference, the note is advisory",
          a10 != a999 and "judge by review" in advisory(a10, a999) and "integer literals" in advisory(a10, a999))
    check("attack: `#if __LINE__ > 0` around a changed literal is a difference (no __LINE__ pinning exists)",
          tk("int answer = 10;") != tk("int answer = 999;"))

    print("-- compile commands")
    check("Windows splitting: \\\" is a literal quote, as the C runtime reads it (shlex kept the backslashes)",
          crt_split(r'cc -DX=\"C:/d\" -c a.cpp') == ["cc", '-DX="C:/d"', "-c", "a.cpp"]
          and crt_split(r'cc "-DY=a b" x\\y "q\\" z') == ["cc", "-DY=a b", r"x\\y", "q\\", "z"])
    check("attack: `\"\"` inside quotes (C runtimes disagree) and an unterminated quote are refused",
          raises(crt_split, 'cc "a""b"') and raises(crt_split, 'cc "abc'))
    check("the program name takes quotes only, as Windows does",
          crt_split(r'"C:\Program Files\cc.exe" -c a.cpp') == [r"C:\Program Files\cc.exe", "-c", "a.cpp"])
    check("POSIX splitting refuses shell metacharacters ($, backticks, redirections)",
          raises(split_command, "cc -DX=$HOME -c a.c", False) and raises(split_command, "cc -c a.c > x", False)
          and split_command("cc '-DX=a b' -c a.c", False) == ["cc", "-DX=a b", "-c", "a.c"])
    names = {"clang++.exe": "gnu", "CLANG_~1.EXE".lower(): "-", "x86_64-w64-mingw32-g++.exe": "gnu",
             "clang-18": "gnu", "cc": "gnu", "cl.exe": "msvc", "clang-cl.exe": "msvc", "python.exe": "-"}
    check("driver names: GNU clang / gcc forms match, cl / clang-cl are MSVC, others are neither",
          all(("gnu" if GNU_DRIVER.match(k) else "msvc" if MSVC_DRIVER.match(k) else "-") == v
              for k, v in names.items()))
    b, root = "/b", "/s"
    cmd = lambda src, obj, extra="": "cc %s-MD -MT %s -MF %s.d -o %s -c %s" % (extra, obj, obj, obj, src)  # noqa
    ea = {"directory": b, "file": "/s/a.cpp", "output": "a.obj", "command": cmd("/s/a.cpp", "a.obj")}
    eb = {"directory": b, "file": "/s/b.cpp", "output": "b.obj", "command": cmd("/s/b.cpp", "b.obj")}
    link = {"directory": b, "file": "a.obj", "output": "x.exe", "command": "cc a.obj -o x.exe"}
    custom = {"directory": b, "file": "/s/gen.py", "output": "x.inc", "command": 'cmd.exe /C "python /s/gen.py"'}
    phony = {"directory": b, "file": "x.inc", "output": "all", "command": ""}

    def classify(e):  # the real classifier, splitting as POSIX does and taking the driver by name (no file system)
        return classify_entry(e, lambda c: split_command(c, False), lambda a0: os.path.basename(a0).lower())

    cases = (("a compile edge", ea, "unit"), ("a link", link, "other"), ("a custom command", custom, "other"),
             ("a phony edge", phony, "other"),
             ("attack: a compile hidden in cmd.exe /C", 'cmd.exe /C "cc -c /s/a.cpp -o a.obj"', "out"),
             ("attack: a C/C++ source compiled and linked without -c", "cc /s/a.cpp -o a.exe", "out"),
             ("an MSVC-style compile", "cl /c /s/a.cpp /Foa.obj", "out"),
             ("attack: an attached -oz.obj next to -o a.obj", "cc -c /s/a.cpp -o a.obj -oz.obj", "out"),
             ("attack: two -o", "cc -c /s/a.cpp -o a.obj -o b.obj", "out"),
             ("attack: -fmodules", cmd("/s/a.cpp", "a.obj", "-fmodules "), "out"),
             ("attack: a .c++ source", dict(ea, file="/s/b.c++", command=cmd("/s/b.c++", "a.obj")), "out"),
             ("attack: a response file", "cc @a.rsp -c /s/a.cpp -o a.obj", "out"),
             ("attack: -x objective-c++", cmd("/s/a.cpp", "a.obj", "-x objective-c++ "), "out"),
             ("attack (Astra r4): -MD -Wp,-MMD,b.d, a forwarded dependency flag", cmd("/s/a.cpp", "a.obj",
                                                                                       "-Wp,-MMD,b.d "), "out"),
             ("attack: -Xpreprocessor -MMD", cmd("/s/a.cpp", "a.obj", "-Xpreprocessor -MMD "), "out"),
             ("attack: -Xclang -dependency-file -Xclang x.d", cmd("/s/a.cpp", "a.obj",
                                                                  "-Xclang -dependency-file -Xclang x.d "), "out"),
             ("attack: -Xclang=-sys-header-deps", cmd("/s/a.cpp", "a.obj", "-Xclang=-sys-header-deps "), "out"),
             ("attack: -Xarch_x86_64 -MMD", cmd("/s/a.cpp", "a.obj", "-Xarch_x86_64 -MMD "), "out"),
             ("attack: /clang:-MMD", cmd("/s/a.cpp", "a.obj", "/clang:-MMD "), "out"),
             ("attack: --config=deps.cfg", cmd("/s/a.cpp", "a.obj", "--config=deps.cfg "), "out"),
             ("attack: --driver-mode=cl", cmd("/s/a.cpp", "a.obj", "--driver-mode=cl "), "out"),
             ("attack: --write-user-dependencies (the long form of -MMD)", cmd("/s/a.cpp", "a.obj",
                                                                               "--write-user-dependencies "), "out"),
             ("attack: -Xclang -include with no -Xclang <path>", cmd("/s/a.cpp", "a.obj", "-Xclang -include "), "out"),
             ("the build's own -Xclang forms (--dependent-lib=, -emit-pch, -include[-pch] -Xclang <path>)",
              cmd("/s/a.cpp", "a.obj", "-Xclang --dependent-lib=msvcrt -Xclang -include-pch -Xclang /b/p.pch "
                  "-Xclang -include -Xclang /b/p.hxx -Xclang -emit-pch "), "unit"))
    for name, e, ok in cases:
        e = dict(ea, command=e) if isinstance(e, str) else e
        check("%s is classified %s" % (name, ok), classify(e)[0] == ok)

    print("-- coverage and scope")
    db1 = (0, json.dumps([ea, link, custom, phony]))
    db2 = (0, json.dumps([ea, eb, link]))
    ch = {"h.hpp": "changed"}
    t = lambda p: 50 if not p.endswith(".obj") else 100  # noqa: E731 - every input older than the object
    nofb = lambda e, a=None: (False, "no compiler in the self-test")  # noqa: E731
    rec_with = (0, "a.obj: #deps 2, deps mtime 1 (VALID)\n    /s/a.cpp\n    /s/h.hpp\n\n")
    rec_without = (0, "a.obj: #deps 1, deps mtime 1 (VALID)\n    /s/a.cpp\n\n")
    two = "a.obj: #deps 2, deps mtime 1 (VALID)\n    /s/a.cpp\n    /s/h.hpp\n\n"

    full = lambda e, a: ("full", None)  # noqa: E731 - a driver that shows -sys-header-deps for every -MD command

    def D(kind, assumed, deps, db, stat=t, fb=nofb, edges=(), before=None, eff=full):
        return discover(b, root, kind, assumed, deps, db, edges, stat, fb, 2, before, norm, classify, eff)

    u, cov, p = D(ch, {}, (1, ""), db1)
    check("a failing `ninja -t deps` is INCOMPLETE (no empty pass)", p and not u)
    u, cov, p = D(ch, {}, (0, ""), db1)
    check("no deps record and no compiler fallback is INCOMPLETE", any("no read set" in x for x in p))
    u, cov, p = D(ch, {}, rec_without, db1)
    check("a changed header no unit reads is INCOMPLETE",
          any("no unit of this build dir reads" in x for x in p) and any("no affected unit" in x for x in p))
    u, cov, p = D(ch, {"h.hpp": "why"}, rec_without, db1)
    check("--assume-no-codegen still refuses an empty affected set", any("no affected unit" in x for x in p) and not u)
    u, cov, p = D(ch, {}, rec_with, db1)
    check("a VALID current -MD record naming the source finds the reader (link, custom, phony are not units)",
          not p and len(u) == 1 and "units (compile edges, all in scope) scanned: 1" in cov)
    u, cov, p = D(ch, {}, (0, two.replace("VALID", "STALE")), db1, fb=lambda e, a=None: (True, ["/s/a.cpp",
                                                                                             "/s/h.hpp"]))
    check("a STALE record takes the compiler's read set", not p and len(u) == 1 and u[0]["how"].startswith("compiler"))
    u, cov, p = D(ch, {}, rec_with, db1, stat=lambda q: 200 if q.endswith("h.hpp") else t(q))
    check("a dependency newer than the object is not trusted", any("no read set" in x for x in p))
    u, cov, p = D(ch, {}, (0, "a.obj: #deps 3, deps mtime 1 (VALID)\n    /s/a.cpp\n\n"), db1)
    check("a record missing deps lines is INCOMPLETE", p and not u)
    u, cov, p = D(ch, {}, (0, "garbage\n"), db1)
    check("an unparsable deps listing is INCOMPLETE", p and not u)
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 0, deps mtime 1 (VALID)\n\n"), db2)
    check("a VALID `#deps 0` record is not a read set (b.obj unresolved -> INCOMPLETE)",
          any("b.obj (compiler -M (deps record empty))" in x for x in p))
    u, cov, p = D(ch, {}, (0, "a.obj: #deps 1, deps mtime 1 (VALID)\n    /s/h.hpp\n\n"), db1)
    check("a record that does not name the source is not trusted", any("does not name the source" in x for x in p))
    md = {"b.obj: ": False, "b.obj: /s/h.hpp": False, "b.obj: /s/b.cpp \\\n /s/h.hpp\n": True}
    check("an -M output is evidence only when it names the source (`b.obj: ` alone is not)",
          all(read_set_from_make(k, eb, norm)[0] == v for k, v in md.items()))
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 0, deps mtime 1 (VALID)\n\n"), db2,
                  fb=lambda e, a=None: read_set_from_make("b.obj: ", e, norm))
    check("two units: `#deps 0` and an empty `-M` for b.obj -> INCOMPLETE, not a complete coverage",
          any("b.obj" in x and "does not name the unit's source" in x for x in p))
    emmd = dict(eb, command="cc -MMD -MT b.obj -MF b.obj.d -c /s/b.cpp -o b.obj")
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 1, deps mtime 1 (VALID)\n    /s/b.cpp\n\n"),
                  (0, json.dumps([ea, emmd])), fb=lambda e, a=None: (True, ["/s/b.cpp", "/s/h.hpp"]))
    check("attack: a -MMD record (no system headers) is not trusted - b.cpp's full -M finds it reads h.hpp",
          not p and sorted(x["src"] for x in u) == ["/s/a.cpp", "/s/b.cpp"])
    enomode = dict(eb, command="cc -c /s/b.cpp -o b.obj")
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 1, deps mtime 1 (VALID)\n    /s/b.cpp\n\n"),
                  (0, json.dumps([ea, enomode])), fb=lambda e, a=None: (True, ["/s/b.cpp", "/s/h.hpp"]))
    check("... and neither is a record whose deps mode the command does not show", not p and len(u) == 2)
    dl = {"opt.hpp": "deleted", "a.cpp": "changed"}
    reads_a = (0, "a.obj: #deps 1, deps mtime 1 (VALID)\n    /s/a.cpp\n\nb.obj: #deps 1, deps mtime 1 (VALID)\n"
                  "    /s/b.cpp\n\n")
    u, cov, p = D(dl, {}, reads_a, db2)
    check("a deleted file without BEFORE read sets is INCOMPLETE", any("no BEFORE read sets" in x for x in p))
    before = {"/b/a.obj": {"/s/a.cpp"}, "/b/b.obj": {"/s/b.cpp", "/s/opt.hpp"}}
    u, cov, p = D(dl, {}, reads_a, db2, before=before)
    check("a deleted __has_include'd header reaches its BEFORE reader (b.cpp is compared)",
          not p and sorted(x["src"] for x in u) == ["/s/a.cpp", "/s/b.cpp"])
    gen = {"h.hpp": "changed", "tools/cloth_capacity.py": "changed"}
    u, cov, p = D(gen, {}, rec_with, db1)
    check("a changed generator script nobody reads is INCOMPLETE without --assume-no-codegen",
          any("cloth_capacity.py" in x and "not modelled" in x for x in p))
    u, cov, p = D(gen, {"tools/cloth_capacity.py": "test reason"}, rec_with, db1)
    check("... and recorded with its reason when assumed", not p and any("test reason" in c for c in cov))
    u, cov, p = D({"h.hpp": "changed"}, {"h.hpp": "x"}, rec_with, db1)
    check("--assume-no-codegen on a file a unit reads is INCOMPLETE", any("but 1 unit(s) read it" in x for x in p))
    edges = ninja_edges("build generated/x.inc: CUSTOM_COMMAND C$:/s/gen.py /s/h.hpp || other\n"
                        "build a.obj: CXX /s/a.cpp | generated/x.inc\n", b)
    u, cov, p = D({"h.hpp": "changed"}, {}, rec_with, db1, edges=edges)
    check("attack: a changed header that a generator also reads is INCOMPLETE (a generated input would go stale)",
          any("non-compile build edge (generated/x.inc)" in x for x in p))
    u, cov, p = D({"gen.py": "changed", "h.hpp": "changed"}, {"gen.py": "says no codegen"}, rec_with, db1,
                  edges=ninja_edges("build generated/x.inc: CUSTOM_COMMAND /s/x/../gen.py\n", b))
    check("attack: --assume-no-codegen cannot excuse a declared generator input",
          any("gen.py is consumed by a non-compile build edge" in x for x in p))
    edges = ninja_edges("build alias: phony /s/h.hpp\nbuild gen.inc: CUSTOM_COMMAND tool.py | alias\n"
                        "build a.obj: CXX /s/a.cpp\n", b)
    u, cov, p = D({"h.hpp": "changed"}, {}, rec_with, db1, edges=edges)
    check("attack: a generator that reaches the changed header through a phony alias is INCOMPLETE",
          any("non-compile build edge (gen.inc)" in x for x in p))
    rc = "app.res: #deps 2, deps mtime 1 (VALID)\n    /s/app.rc\n    /s/h.hpp\n\n"
    u, cov, p = D({"h.hpp": "changed"}, {}, (0, two + rc), db1)
    check("attack: a resource compile whose deps record lists the changed header is INCOMPLETE",
          any("app.res (its deps record)" in x for x in p))
    check("ninja_edges reads escapes, continuations, variables and implicit / order-only inputs",
          ninja_edges("w = C$:/x/\nbuild o$ 1.obj: R ${w}a$ b.cpp $\n  | $w/i.h || oo\n", b)
          == [(["o 1.obj"], "R", ["C:/x/a b.cpp", "C:/x//i.h", "oo"])])
    u, cov, p = D({"x.def": "changed"}, {},
                  (0, "a.obj: #deps 2, deps mtime 1 (VALID)\n    /s/a.cpp\n    /s/x.def\n\n"), db1)
    check("a changed .def a unit includes is a modelled input (no flag needed)", not p and len(u) == 1)
    a, p = resolve_assumed(["docs/*.md=documentation", "nothing/*=x", "noreason="], ["docs/A.md", "k/a.cpp"])
    check("--assume-no-codegen: patterns resolve, an unmatched pattern and a missing reason are INCOMPLETE",
          a == {"docs/A.md": "documentation"} and len(p) == 2)
    u, cov, p = D(ch, {}, rec_with, (0, json.dumps([ea, dict(ea, command='cmd.exe /C "cc -c /s/a.cpp -o a.obj"',
                                                             output="z.obj")])))
    check("a wrapped compile in the database is INCOMPLETE (not skipped)", any("out of scope" in x for x in p))
    u, cov, p = D(ch, {}, rec_with, (0, json.dumps([ea, dict(ea, file="/s/c.cpp")])))
    check("attack: two compile edges writing one object are INCOMPLETE", any("two compile edges write" in x for x in p))
    ewp = dict(eb, command=cmd("/s/b.cpp", "b.obj", "-Wp,-MMD,b.d "))
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 1, deps mtime 1 (VALID)\n    /s/b.cpp\n\n"),
                  (0, json.dumps([ea, ewp])))
    check("attack (Astra r4): a.cpp reads h.hpp; b.cpp's -MD -Wp,-MMD,b.d record omits it - INCOMPLETE, not a pass "
          "on a.cpp alone", any("b.obj" in x and "-Wp,-MMD,b.d" in x and "out of scope" in x for x in p))
    hide = lambda e, a: ("full", None) if "/s/b.cpp" not in a else ("partial", "mock: no -sys-header-deps")  # noqa
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 1, deps mtime 1 (VALID)\n    /s/b.cpp\n\n"), db2, eff=hide,
                  fb=lambda e, a=None: (True, ["/s/b.cpp", "/s/h.hpp"]))
    check("attack: a visible -MD whose effective mode omits system headers is not trusted - b.cpp's full -M finds "
          "it reads h.hpp", not p and sorted(x["src"] for x in u) == ["/s/a.cpp", "/s/b.cpp"]
          and any("does not show full deps" in c for c in cov))

    print("-- the effective deps mode: clang -### read positionally (Astra r5)")
    q = lambda *a: " ".join('"%s"' % x.replace("\\", "\\\\").replace('"', '\\"') for x in a)  # noqa: E731
    hdr = 'clang version 22.1.6\nTarget: x86_64-pc-windows-msvc\nThread model: posix\nInstalledDir: C:\\LLVM\\bin\n'
    pre = ("C:\\LLVM\\bin\\clang++.exe", "-cc1", "-triple", "x86_64-pc-windows-msvc", "-emit-obj")
    post = ("-internal-isystem", "C:\\LLVM\\lib\\clang\\22\\include", "-O3", "-o", "a.obj", "-x", "c++", "a.cpp")
    job = lambda *deps: hdr + " (in-process)\n " + q(*(pre + deps + post)) + "\n"  # noqa: E731
    H = classify_hash_output
    check("a plain -MD job (-dependency-file F -MT T -sys-header-deps) is full",
          H(job("-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps"))[0] == "full")
    check("... with an escaped -MQ target (\\$\\$x) and two targets, still full",
          H(job("-dependency-file", "a.d", "-MT", "$$x", "-MT", "a.obj", "-sys-header-deps"))[0] == "full"
          and cc1_argv(' "-MT" "\\$\\$x"') == ["-MT", "$$x"])
    check("a -MMD job (no -sys-header-deps after the targets) is partial",
          H(job("-dependency-file", "a.d", "-MT", "a.obj"))[0] == "partial")
    check("attack (Astra r5): implicit -Wp,-MMD + visible -MD -I -sys-header-deps - the include path named "
          "-sys-header-deps is not the flag: unknown (exit 2)",
          H(job("-dependency-file", "b.d", "-MT", "a.obj", "-I", "-sys-header-deps"))[0] == "unknown")
    check("attack (Astra r5): implicit -Wp,-MMD + visible -MT -sys-header-deps - a target named -sys-header-deps "
          "is a value: not full",
          H(job("-dependency-file", "b.d", "-MT", "-sys-header-deps", "-MT", "a.obj"))[0] == "partial")
    check("... while the same targets with the real flag after them are full",
          H(job("-dependency-file", "a.d", "-MT", "-sys-header-deps", "-MT", "a.obj", "-sys-header-deps"))[0]
          == "full")
    check("attack: a -dependency-file / -sys-header-deps token anywhere else in the job is unknown (exit 2)",
          H(job("-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps", "-isystem", "-dependency-file"))[0]
          == "unknown"
          and H(job("-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps", "-D", "-sys-header-deps"))[0]
          == "unknown")
    check("attack (Astra r5): a configuration file in use (clang.cfg next to the driver, a target-prefixed one, "
          "--config) is unknown (exit 2), even with a full-looking job",
          H(hdr + "Configuration file: C:\\LLVM\\bin\\clang++.cfg\n" + job(
              "-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps").split(hdr, 1)[1])[0] == "unknown")
    check("a job line with unquoted text, no cc1 job, or two cc1 jobs is unknown",
          H(hdr + ' "clang" "-cc1" -I "-dependency-file" "a.d" "-MT" "a.obj" "-sys-header-deps"\n')[0] == "unknown"
          and H(hdr)[0] == "unknown"
          and H(job("-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps") * 2)[0] == "unknown")
    unk = lambda e, a: ("full", None) if "/s/b.cpp" not in a else ("unknown", "mock: -I -sys-header-deps")  # noqa
    u, cov, p = D(ch, {}, (0, two + "b.obj: #deps 1, deps mtime 1 (VALID)\n    /s/b.cpp\n\n"), db2, eff=unk,
                  fb=lambda e, a=None: (True, ["/s/b.cpp"]))
    check("attack (Astra r5, the receipts-level false pass): a.cpp reads h.hpp; b.cpp's mode is unknown - "
          "INCOMPLETE, not a pass on a.cpp alone", any("b.obj" in x and "cannot be established" in x for x in p))
    check("CLANG_NO_DEFAULT_CONFIG in the tool's environment is INCOMPLETE (it would hide the build's config file)",
          len(env_problems({"CLANG_NO_DEFAULT_CONFIG": "1"})) == 1)
    eh = dict(ea, file="/s/p.cxx", output="p.pch", command=cmd("/s/p.cxx", "p.pch", "-x c++-header "))
    epch = dict(eb, command=cmd("/s/b.cpp", "b.obj", "-Xclang -include-pch -Xclang p.pch "))
    pdeps = ("p.pch: #deps 2, deps mtime 1 (VALID)\n    /s/p.cxx\n    /s/h.hpp\n\n"
             "b.obj: #deps 2, deps mtime 1 (VALID)\n    /s/b.cpp\n    /b/p.pch\n\n")
    tp = lambda q: 100 if q.endswith((".obj", ".pch")) else 50  # noqa: E731
    u, cov, p = D(ch, {}, (0, pdeps), (0, json.dumps([eh, epch])), stat=tp)
    check("attack: a changed header inside a precompiled header a unit uses is INCOMPLETE",
          any("inside the precompiled header" in x for x in p))
    u, cov, p = D(ch, {}, (0, two), (0, json.dumps([ea, dict(epch, command=cmd(
        "/s/b.cpp", "b.obj", "-include-pch /b/missing.pch "))])), fb=lambda e, a=None: (True, ["/s/b.cpp"]))
    check("attack: a precompiled header no compile edge builds is INCOMPLETE",
          any("which no compile edge in scope builds" in x for x in p))
    u, cov, p = D(ch, {}, (0, "a.obj: #deps 2, deps mtime 1 (VALID)\n    /s/A.cpp\n    /s/x/../h.hpp\n\n"), db1)
    check("attack: a deps record spelling a path differently (case, ..) still names the same file",
          not p and len(u) == 1)
    uu = {"how": "ninja deps", "reads": {"/s/a.cpp", "/s/h.hpp"}}
    check("attack: an AFTER preprocessing that reads another file set than the build did is FAILED",
          read_set_mismatch(uu, {"/s/a.cpp", "/s/h.hpp", "/s/env/h2.hpp"}) is not None
          and read_set_mismatch(uu, {"/s/a.cpp", "/s/h.hpp"}) is None)
    up = {"how": "ninja deps", "reads": {"/s/a.cpp", "/b/p.pch"},
          "pch_reads": {"/s/p.cxx", "/s/p.h", "/r/include/stddef.h", "/sys/ucrt/stddef.h", "/sys/ucrt/other.h",
                        "/s/u/stddef.h", "/sys/ucrt/q.h", "/s/blob.bin"}}
    probed_seen = {"/s/a.cpp", "/b/p.pch", "/sys/ucrt/stddef.h"}
    texts = {"/r/include/stddef.h": "#if defined(__MVS__) && __has_include_next(<stddef.h>)\n#endif\n",
             "/s/p.h": "#if __has_include_next(<q.h>)\n#endif\n#if __has_include(\"/s/blob.bin\")\n#endif\n"}
    wit = lambda ent: (lambda: {"entered": ent, "sysdirs": ["/r/include", "/sys/ucrt"], "resource": "/r"})  # noqa
    good = wit({"/s/a.cpp", "/s/p.h", "/r/include/stddef.h"})
    M = lambda seen, w=good: read_set_mismatch(up, seen, w, texts.get)  # noqa: E731
    with_extra = lambda f: {"/s/a.cpp", "/b/p.pch", f}  # noqa: E731
    check("real use: the UCRT stddef.h - in the PCH's record, unentered, under a system include dir, probed by the "
          "entered builtin stddef.h's __has_include_next(<stddef.h>) - is explained, and named",
          M(probed_seen) is None and up.get("pch_explained") == ["/sys/ucrt/stddef.h"])
    check("attack (Astra, receipts-fix2): the same file ENTERED by the unit's preprocessing (include-resolution drift "
          "the PCH's record would hide) is FAILED",
          M(probed_seen, wit({"/s/a.cpp", "/r/include/stddef.h", "/sys/ucrt/stddef.h"})) is not None)
    check("... and so is the file when the unit's preprocessing cannot be witnessed (or is not asked)",
          M(probed_seen, lambda: None) is not None and read_set_mismatch(up, probed_seen) is not None)
    check("attack (Astra, receipts-fix2 r3): an extra file outside every system include directory is FAILED",
          M(with_extra("/s/u/stddef.h")) is not None)
    check("attack (Astra, receipts-fix2 r3): a non-builtin probe is no longer excused (/s/p.h's "
          "__has_include_next(<q.h>), a user header's __has_include)",
          M(with_extra("/sys/ucrt/q.h")) is not None and M(with_extra("/s/blob.bin")) is not None)
    check("... nor a system header no entered builtin header probes, nor one whose builtin prober was not entered",
          M(with_extra("/sys/ucrt/other.h")) is not None
          and M(probed_seen, wit({"/s/a.cpp", "/s/p.h"})) is not None)
    calls = []
    read_set_mismatch(dict(up, pch_reads=set()), probed_seen, lambda: calls.append(1))
    check("... and the witness is asked only for a PCH-explained difference", not calls)
    etxt = {"/s/e1.cpp": 'int b[] = {\n  #  embed "blob.bin"\n};\n', "/s/e2.h": "%:embed <x.bin>\n",
            "/s/e3.h": "#\\\nembed <x.bin>\n", "/s/ok.h": "#define embedded 1\n// #embed\nint embed;\n#embedded\n"}
    check("attack (Astra, receipts-fix2 r3): an #embed directive in any entered file is found (any whitespace, %:, "
          "a line splice), and nothing else is",
          all(embed_in({f}, etxt.get) == f for f in ("/s/e1.cpp", "/s/e2.h", "/s/e3.h"))
          and embed_in({"/s/ok.h"}, etxt.get) is None)
    dE = ['#embed "/sys/ucrt/stddef.h" /* clang -E -dE */']
    check("attack (Astra, receipts-fix2 r4): clang's -dE answer decides - an #embed line it printed is FAILED, no -dE "
          "answer is FAILED, the text scan is only an extra",
          "clang -dE" in (marker_problem({"/s/ok.h"}, dE, etxt.get) or "")
          and marker_problem({"/s/ok.h"}, None, etxt.get) is not None
          and "#embed" in (marker_problem({"/s/ok.h", "/s/e1.cpp"}, [], etxt.get) or "")
          and marker_problem(None, [], etxt.get) is not None and marker_problem({"/s/ok.h"}, [], etxt.get) is None)
    fd, tmpo = tempfile.mkstemp(prefix="pp_receipts_dE_")
    os.write(fd, b'# 1 "a.cpp"\nint b[] = {\n#embed "x.bin" /* clang -E -dE */\n};\nint embed;\n#embedded\n')
    os.close(fd)
    try:
        el = embed_lines(tmpo)
    finally:
        os.remove(tmpo)
    check("... embed_lines() reads clang's canonical #embed lines and nothing else; an unreadable output is no answer",
          el == ['#embed "x.bin" /* clang -E -dE */'] and embed_lines(tmpo) is None)
    eu = {"directory": "/b", "file": "/s/a.cpp"}
    check("attack (Astra, receipts-fix2 r2): zero line markers are no evidence - the markers must name the unit's own "
          "source, else no answer (FAILED)",
          markers_entered("", eu, norm) is None and markers_entered('int a;\n# 1 "/s/h.h" 1\n', eu, norm) is None
          and markers_entered('# 1 "/s/a.cpp"\n# 1 "/s/h.h" 1\n#line 3 "/s/a.cpp"\n', eu, norm)
          == {"/s/a.cpp", "/s/h.h"})
    check("attack (Astra, receipts-fix2 r2): the marker pass drops -P and every other -E output-shaping option from "
          "its own command, and adds clang's cc1 -dE (the driver's -dE is dropped silently)",
          not set(marker_command(["cc", "-P", "-dM", "-C", "-dE", "-c", "a.cpp", "-o", "a.obj"], "x")[:-5])
          & set(PP_OUTPUT_FLAGS) and marker_command(["cc", "-c", "a.cpp", "-o", "a.obj"], "x")[-5:]
          == ["-E", "-Xclang", "-dE", "-o", "x"])
    check("... and a unit whose command has such an option (-P, -dM, -C, -fdirectives-only) is out of scope",
          all(classify(dict(ea, command=cmd("/s/a.cpp", "a.obj", f + " ")))[0] == "out"
              for f in ("-P", "-dM", "-C", "-fdirectives-only")))
    check("... but a file in neither record is still FAILED, even next to an explained one",
          M(probed_seen | {"/s/env/h2.hpp"}) is not None)
    check("... and so is a file the unit's record lists that the preprocessing did not read",
          M({"/b/p.pch", "/sys/ucrt/stddef.h"}) is not None)
    check("... and so is the explained file when the PCH's read set did not come from its build record",
          read_set_mismatch(dict(up, pch_reads=None), probed_seen, good, texts.get) is not None)
    check("attack (Astra, receipts-fix2): several or empty -include-pch options are out of scope (clang uses the last; "
          "`-include-pch x.pch -include-pch \"\"` loads none)",
          classify(dict(ea, command=cmd("/s/a.cpp", "a.obj", "-include-pch /b/x.pch -include-pch /b/y.pch ")))[0]
          == "out" and classify(dict(ea, command=cmd("/s/a.cpp", "a.obj", "-Xclang -include-pch -Xclang /b/x.pch "
                                                                           "-Xclang -include-pch -Xclang /b/y.pch ")))[0]
          == "out" and pch_options(["cc", "-include-pch=/b/x.pch", "-Xclang", "-include-pch", "-Xclang", ""])
          == ["/b/x.pch", ""])
    pj = lambda *pch: job("-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps", *pch)  # noqa: E731
    check("attack (Astra, receipts-fix2): the cc1 job's precompiled header must be exactly the command's - two, an "
          "empty one, an implicit one (-include x.h next to x.h.pch), or none where one is shown is unknown",
          H(pj("-include-pch", "/b/x.pch"), "/b/x.pch", "/b")[0] == "full"
          and H(pj("-include-pch", "x.pch"), "/b/x.pch", "/b", norm)[0] == "full"
          and H(pj("-include-pch", "/b/x.pch", "-include-pch", ""), "/b/x.pch", "/b")[0] == "unknown"
          and H(pj("-include-pch", "/b/x.h.pch"), None, "/b")[0] == "unknown"
          and H(pj(), "/b/x.pch", "/b")[0] == "unknown" and H(pj())[0] == "full")
    eh2 = dict(ea, file="/s/p.cxx", output="p.pch", command=cmd("/s/p.cxx", "p.pch", "-x c++-header "))
    ec2 = dict(eb, command=cmd("/s/b.cpp", "b.obj", "-Xclang -include-pch -Xclang /b/p.pch "))
    prec = "p.pch: #deps 2, deps mtime 1 (VALID)\n    /s/p.cxx\n    /s/u.h\n\n"
    crec = "b.obj: #deps 3, deps mtime 1 (VALID)\n    /s/b.cpp\n    /b/p.pch\n    /s/k.hpp\n\n"
    u, cov, p = D({"k.hpp": "changed"}, {}, (0, prec + crec), (0, json.dumps([eh2, ec2])), stat=tp)
    check("discover hands a consumer its PCH's trusted build record (for the cross-check)",
          not p and len(u) == 1 and u[0].get("pch_reads") == {"/s/p.cxx", "/s/u.h"})
    u, cov, p = D({"k.hpp": "changed"}, {}, (0, prec.replace("VALID", "STALE") + crec), (0, json.dumps([eh2, ec2])),
                  stat=tp, fb=lambda e, a=None: (True, ["/s/p.cxx", "/s/u.h"]))
    check("... and nothing when the PCH's read set came from the compiler's -M instead",
          not p and len(u) == 1 and u[0].get("pch_reads", "absent") is None)
    try:
        n = receipt_names(["/b/sub/a.obj", "/b/sub__a.obj", "/b/Sub/A.obj"], "/b")
    except ValueError:
        n = {}
    check("attack: receipt names of sub/a.obj, sub__a.obj, Sub/A.obj are distinct (the flattened names collided)",
          len(set(v.lower() for v in n.values())) == 3)
    check("attack: driver environment variables (CPATH, CCC_OVERRIDE_OPTIONS, INCLUDE) are INCOMPLETE",
          len(env_problems({"CPATH": "/x", "CCC_OVERRIDE_OPTIONS": "+-DX", "INCLUDE": "C:/i", "PATH": "x"})) == 3)
    check("attack: changed paths that differ only in case are INCOMPLETE",
          path_problems(["Kernel/a.h", "kernel/a.h"]) and not path_problems(["kernel/a.h", "kernel/b.h"]))

    if cc:
        print("-- compiler-backed (%s)" % cc)
        d = tempfile.mkdtemp(prefix="pp_receipts_selftest_")
        try:
            e = {"directory": d, "file": "a.cpp", "output": "a.obj", "arguments": [cc, "-c", "a.cpp", "-o", "a.obj"]}

            def pp(src, entry=e):
                open(os.path.join(d, "a.cpp"), "w", encoding="utf-8").write(src)
                ok, err, reads = preprocess(entry, os.path.join(d, "a.i"))
                return tk(read_text(os.path.join(d, "a.i"))) if ok else None

            s1 = "#if __LINE__ > 0\nint answer = 10;\n#endif\n"
            s2 = "#if __LINE__ > 0\nint answer = 999;\n#endif\n"
            n1, n2 = pp(s1), pp(s2)
            check("compiler: Astra's `#if __LINE__ > 0` change is DIFF", None not in (n1, n2) and n1 != n2)
            w1 = pp("#define W(x) w(__FILE__, __LINE__, x)\nint a = W(1);\n")
            w2 = pp("#define W(x) w(__FILE__, __LINE__, x)\n\n\nint a = W(1);\n")
            check("compiler: lines moved under __LINE__ are DIFF too (advisory: integer literals only)",
                  None not in (w1, w2) and w1 != w2 and "integer literals" in advisory(w1, w2))
            r1, r2 = pp('auto s = R""(a\n\nb)"";\n'), pp('auto s = R""(a\nb)"";\n')
            check("compiler: R\"\"(...)\"\" literals with and without a blank line differ", r1 and r2 and r1 != r2)
            m1, m2, m3 = pp("#define M -\nint x = 1 M-1;\n"), pp("int x = 1 --1;\n"), pp("int x = 1 - -1;\n")
            check("compiler: -E keeps a macro's tokens apart as the compiler does (M-1 is - - 1, not -- 1)",
                  None not in (m1, m2, m3) and m1 != m2 and m1 == m3)
            ok = pp("int a;") is not None
            txt = read_text(os.path.join(d, "a.i")) if ok else ""
            check("compiler: -E -P output carries no line markers", ok and not LINE_MARKER.search(txt))
            open(os.path.join(d, "h.h"), "w").write("int h;\n")
            open(os.path.join(d, "a.cpp"), "w").write('#include "h.h"\n')
            ok, err, reads = preprocess(e, os.path.join(d, "a.i"))
            check("compiler: -E with -MD yields the read set (source and header)",
                  ok and canon(os.path.join(d, "h.h")) in reads and canon(os.path.join(d, "a.cpp")) in reads)
            ok, why = has_include_recorded(cc)
            check("compiler: it lists a file found only by __has_include in -M (else a new / deleted file is "
                  "INCOMPLETE)", ok)
            q = dict(e, arguments=None, command='"%s" "-DXQ=\\"a  b\\"" -c a.cpp -o a.obj' % cc)
            check("compiler: a define with \\\" inside quotes reaches the compiler as the build passes it",
                  pp("const char* s = XQ;\n", q) == ["const", "char", "*", "s", "=", '"a  b"', ";"]
                  if os.name == "nt" else True)
            if os.name != "nt":
                print("skip the Windows quoting check (not Windows)")
            mdf = [cc, "-MD", "-MT", "a.obj", "-MF", "a.obj.d", "-c", "a.cpp", "-o", "a.obj"]
            if is_clang(cc):
                m1 = deps_effective(e, mdf)[0]
                m2 = deps_effective(e, mdf[:-4] + ["-Wp,-MMD,b.d"] + mdf[-4:])[0]
                m3 = deps_effective(e, [cc, "-MMD", "-MF", "a.obj.d", "-c", "a.cpp", "-o", "a.obj"])[0]
                m4 = deps_effective(e, strip_outputs(mdf) + ["-M"])[0]
                check("compiler: clang -### shows full deps for -MD and for -M, not for -MD -Wp,-MMD,b.d or -MMD",
                      (m1, m4) == ("full", "full") and "full" not in (m2, m3))
                # an implicit config file modelled by prepending its contents (-Wp,-MMD,b.d) to the visible flags
                hidden = [cc, "-Wp,-MMD,b.d", "-MD"]
                m5 = deps_effective(e, hidden + ["-I", "-sys-header-deps"] + mdf[2:])
                m6 = deps_effective(e, hidden + ["-MT", "-sys-header-deps"] + mdf[2:])
                check("compiler (Astra r5): prepended -Wp,-MMD,b.d with visible -MD -I -sys-header-deps is unknown, "
                      "with -MT -sys-header-deps not full", m5[0] == "unknown" and m6[0] != "full")
                cfg = os.path.join(d, "hidden.cfg")
                open(cfg, "w").write("-Wp,-MMD,b.d\n")
                m7 = deps_effective(e, [cc, "--config=" + cfg] + mdf[1:])
                check("compiler (Astra r5): a configuration file in use is unknown, whatever the job shows",
                      m7[0] == "unknown" and "configuration file" in (m7[1] or ""))
                ok, files = compiler_deps(dict(e, file="a.cpp"), [cc, "-c", "a.cpp", "-o", "a.obj"])
                check("compiler: compiler_deps passes its own -### check and names the source", ok)
            else:
                print("skip the -### checks (not clang: a non-clang driver's deps records are never trusted)")
            if is_clang(cc):
                # real use (the backend lane's 2 FAILED units): a PCH consumer's -E read set has a file the PCH build
                # only probed; its -c record does not (a PCH's input files are the files it entered). Only clang's
                # builtin headers' __has_include_next probes are excused; a user header's __has_include is not.
                for n, body in (("probed_only.h", "#error never entered\n"), ("p.cxx", "\n"),
                                ("c.cpp", "int c = HP;\n"), ("ph.h", "int ph_v;\n"), ("c2.cpp", '#include "ph.h"\n'),
                                ("c3.cpp", 'const unsigned char b[] = {\n#embed "blob.bin"\n};\n'),
                                ("p.h", '#if __has_include("probed_only.h")\n#define HP 1\n#endif\n'
                                        '#if __has_include("ph.h")\n#endif\n#if __has_include("blob.bin")\n#endif\n'
                                        '#include <stddef.h>\n')):
                    open(os.path.join(d, n), "w").write(body)
                open(os.path.join(d, "blob.bin"), "wb").write(b"AB")
                ph = ["-Xclang", "-include", "-Xclang", os.path.join(d, "p.h")]
                run([cc, "-Xclang", "-emit-pch"] + ph + ["-x", "c++-header", "-MD", "-MT", "p.pch", "-MF", "p.pch.d",
                                                         "-o", "p.pch", "-c", "p.cxx"], cwd=d)
                use = [cc, "-Xclang", "-include-pch", "-Xclang", os.path.join(d, "p.pch")] + ph
                run(use + ["-MD", "-MT", "c.obj", "-MF", "c.obj.d", "-o", "c.obj", "-c", "c.cpp"], cwd=d)
                rd = lambda n: open(os.path.join(d, n)).read() if os.path.exists(os.path.join(d, n)) else ""  # noqa
                ue = {"directory": d, "file": "c.cpp", "output": "c.obj"}
                okc, rec = read_set_from_make(rd("c.obj.d"), ue)
                okp, prec = read_set_from_make(rd("p.pch.d"), dict(ue, file="p.cxx"))
                ok, err, seen = preprocess(ue, os.path.join(d, "c.i"), use + ["-c", "c.cpp", "-o", "c.obj"])
                probed = canon(os.path.join(d, "probed_only.h"))
                uc = {"how": "ninja deps", "reads": {canon(x) for x in rec} if okc else set(),
                      "pch_reads": {canon(x) for x in prec} if okp else set()}
                ent = lambda e2, a2, n: (lambda: unit_witness(e2, a2, entered_files(e2, a2, os.path.join(d, n))))  # noqa
                cargs = use + ["-c", "c.cpp", "-o", "c.obj"]
                wc = ent(ue, cargs, "c.lm")()
                check("compiler (real use): a file the PCH build only probed is in the consumer's -E read set, not "
                      "in its -c record and has no line marker - but a user header's __has_include is not excused "
                      "(Astra, receipts-fix2 r3)",
                      ok and okc and okp and wc and probed in seen - uc["reads"] and probed in uc["pch_reads"]
                      and probed not in wc["entered"] and read_set_mismatch(uc, seen, lambda: wc) is not None)
                extra = (seen or set()) - uc["reads"]
                nxt = sorted(f for f in extra if f.endswith("/stddef.h") and wc
                             and any(f.startswith(sd.rstrip("/") + "/") for sd in wc["sysdirs"]))
                if nxt:
                    only = dict(uc, reads=uc["reads"] | (extra - set(nxt)))
                    check("compiler (real use): the next stddef.h that clang's builtin stddef.h probes with "
                          "__has_include_next (%s) is explained" % nxt[0],
                          read_set_mismatch(only, seen, lambda: wc) is None and only.get("pch_explained") == nxt)
                    check("compiler (real use): ... and FAILED without the PCH's record (the backend lane's case)",
                          read_set_mismatch(dict(only, pch_reads=None), seen, lambda: wc) is not None)
                else:
                    print("skip the builtin-probe check: clang's stddef.h found no next stddef.h here")
                # Astra's negative: the PCH probes ph.h; c2.cpp includes ph.h; its record without ph.h stands for an
                # include that resolves to ph.h in the receipt's environment only
                u2 = {"directory": d, "file": "c2.cpp", "output": "c2.obj"}
                a2 = use + ["-c", "c2.cpp", "-o", "c2.obj"]
                ok2, err2, seen2 = preprocess(u2, os.path.join(d, "c2.i"), a2)
                hph = canon(os.path.join(d, "ph.h"))
                drift = {"how": "ninja deps", "reads": (seen2 or set()) - {hph}, "pch_reads": uc["pch_reads"]}
                check("compiler (Astra, receipts-fix2): a header the PCH only probed but this unit ENTERS, missing from "
                      "its record, is FAILED", ok2 and hph in uc["pch_reads"]
                      and read_set_mismatch(drift, seen2, ent(u2, a2, "c2.lm")) is not None)
                a2p = a2[:1] + ["-P"] + a2[1:]
                ok2p, err2p, seen2p = preprocess(u2, os.path.join(d, "c2p.i"), a2p)
                msg = read_set_mismatch(dict(drift), seen2p, ent(u2, a2p, "c2p.lm")) if ok2p else None
                check("compiler (Astra, receipts-fix2 r2): with -P in the unit's command the marker pass still shows "
                      "the entered header (FAILED as entered, not excused by zero markers)",
                      ok2p and msg is not None and "entered (a line marker names it)" in msg)
                # Astra's #embed negative: the PCH probes blob.bin; c3.cpp embeds it - no line marker, its bytes in
                # the tokens; with its record lacking blob.bin the old exemption excused it
                u3 = {"directory": d, "file": "c3.cpp", "output": "c3.obj"}
                a3 = use + ["-c", "c3.cpp", "-o", "c3.obj"]
                ok3, err3, seen3 = preprocess(u3, os.path.join(d, "c3.i"), a3)
                hb = canon(os.path.join(d, "blob.bin"))
                e3 = entered_files(u3, a3, os.path.join(d, "c3.lm"))
                emb3 = {"how": "ninja deps", "reads": (seen3 or set()) - {hb}, "pch_reads": uc["pch_reads"]}
                check("compiler (Astra, receipts-fix2 r3): #embed reads blob.bin with no line marker (the hazard), "
                      "clang's -dE marker pass prints it (FAILED before any excuse), and the narrowed excuse refuses "
                      "it too",
                      ok3 and hb in seen3 and e3 is not None and hb not in e3 and hb in uc["pch_reads"]
                      and marker_problem(e3, embed_lines(os.path.join(d, "c3.lm"))) is not None
                      and embed_lines(os.path.join(d, "c3.lm"))
                      and read_set_mismatch(emb3, seen3, ent(u3, a3, "c3.lm")) is not None)
                check("compiler: this clang shows #embed under -Xclang -dE (shows_embeds)", shows_embeds(cc))
                # Astra round 4: spellings a text scan misses - a comment after / before the #, a digraph with a
                # comment, vertical tab, form feed - embedding an ELIGIBLE system header (the next stddef.h the builtin
                # stddef.h probes: under a system include dir, unentered, in the PCH's record); clang's -dE finds all
                target = nxt[0] if nxt else canon(os.path.join(d, "blob.bin"))
                spell = {"#/**/embed": "#/**/embed", "/**/ #embed": "/**/ #embed", "%:/**/embed": "%:/**/embed",
                         "#<VT>embed": "#\vembed", "#<FF>embed": "#\fembed"}
                missed, caught = [], []
                for k, sp in spell.items():
                    src = os.path.join(d, "c4.cpp")
                    open(src, "w", newline="\n").write('#include <stddef.h>\nconst unsigned char b[] = {\n%s "%s"\n};\n'
                                                       % (sp, target))
                    u4 = {"directory": d, "file": "c4.cpp", "output": "c4.obj"}
                    a4 = use + ["-c", "c4.cpp", "-o", "c4.obj"]
                    e4 = entered_files(u4, a4, os.path.join(d, "c4.lm"))
                    el4 = embed_lines(os.path.join(d, "c4.lm"))
                    (caught if marker_problem(e4, el4) is not None and el4 else missed).append(k)
                    if embed_in(e4 or set(), lambda f: open(f, errors="replace").read()
                                if os.path.isfile(f) else None) is None:
                        print("   (the text scan alone misses %s)" % k)
                check("compiler (Astra, receipts-fix2 r4): clang -dE finds #embed in every spelling (%s) of an eligible "
                      "system header (%s) - FAILED, never excused" % (", ".join(spell), os.path.basename(target)),
                      not missed and len(caught) == len(spell))
                mdf2 = ["-MD", "-MT", "c.obj", "-MF", "c.obj.d", "-c", "c.cpp", "-o", "c.obj"]
                m8 = deps_effective(ue, [cc, "-include-pch", os.path.join(d, "p.pch"), "-include-pch", ""] + mdf2)
                open(os.path.join(d, "x.h"), "w").write("int x_v;\n")
                shutil.copyfile(os.path.join(d, "p.pch"), os.path.join(d, "x.h.pch"))
                m9 = deps_effective(ue, [cc, "-include", "x.h"] + mdf2)
                m10 = deps_effective(ue, use + mdf2)
                check("compiler (Astra, receipts-fix2): clang's cc1 job decides the PCH - `-include-pch p.pch "
                      "-include-pch \"\"` and an implicit x.h.pch are unknown, the CMake form is full",
                      (m8[0], m9[0], m10[0]) == ("unknown", "unknown", "full"))
        finally:
            shutil.rmtree(d, ignore_errors=True)
    else:
        print("skip compiler-backed checks (pass --cc <clang++>)")
    print("-- tried and stated in SCOPE rather than tested offline: symlink / junction / 8.3 spellings (canon() "
          "resolves them; a mismatch leaves a changed file unread = INCOMPLETE), __DATE__ / __TIME__ / __TIMESTAMP__ "
          "(DIFF, never EQUAL), compile-command changes (out of scope: unread changed files need --assume-no-codegen), "
          "untracked / assume-unchanged / submodule / symlink entries (tree_problems, git state), a generator's "
          "undeclared inputs and a compiler hidden in a script (not seen)")
    print("pp_receipts self-test: %s (%d failed)%s" % ("PASS" if not bad else "FAIL", len(bad),
                                                      "" if cc else " - compiler-backed checks not run"))
    return 1 if bad else 0


def receipts(a):
    root = os.getcwd()

    def fail(msg):
        print("pp_receipts: INCOMPLETE - " + msg)
        return 2

    if run(GIT + ["rev-parse", "--verify", "--quiet", a.base + "^{commit}"]).returncode:
        return fail("--base %s is not a commit in this repo" % a.base)
    ninja_file = os.path.join(a.build_dir, "build.ninja")
    if not os.path.exists(ninja_file):
        return fail("%s has no build.ninja (build it first)" % a.build_dir)
    d = run(GIT + ["diff", "--no-renames", "--ignore-submodules=none", "--name-only", "-z", a.base])
    if d.returncode:
        return fail("`git diff` exited %d: %s" % (d.returncode, d.stderr.strip()))
    files = [f for f in d.stdout.split("\0") if f.strip()]
    if not files:
        return fail("nothing differs from %s" % a.base)
    dirty = run(GIT + ["diff", "--name-only", "HEAD", "--"] + files)
    if dirty.returncode or dirty.stdout.strip():
        return fail("changed files differ from HEAD (an interrupted run? `git checkout -- <file>`): " +
                    (dirty.stdout.strip().replace("\n", " ") or dirty.stderr.strip()))
    problems = env_problems(os.environ) + tree_problems(files, a.base)
    kind = {}
    for f in files:
        at_base = run(GIT + ["cat-file", "-e", "%s:%s" % (a.base, f)]).returncode == 0
        here = os.path.exists(f)
        if not at_base and not here:
            return fail("%s is neither in %s nor in the tree" % (f, a.base))
        kind[f] = "changed" if at_base and here else "new" if here else "deleted"
    assumed, ap_problems = resolve_assumed(a.assume_no_codegen, files)
    problems += ap_problems
    print("base %s; changed files: %s" % (a.base, " ".join("%s(%s)" % (f, k) if k != "changed" else f
                                                         for f, k in kind.items())))
    bdir = canon(os.path.join(root, a.build_dir))
    deps = run(["ninja", "-C", a.build_dir, "-t", "deps"])
    compdb = run(["ninja", "-C", a.build_dir, "-t", "compdb"])
    try:
        edges = ninja_edges(open(ninja_file, encoding="utf-8", errors="replace").read(), bdir,
                            lambda inc: open(os.path.join(a.build_dir, inc), encoding="utf-8", errors="replace").read())
    except (OSError, ValueError) as ex:
        return fail("build.ninja: %s" % ex)
    before_reads = None
    if not problems and compdb.returncode == 0 and any(k != "changed" for k in kind.values()):
        try:
            units, up = compile_units(json.loads(compdb.stdout), canon)
        except ValueError:
            units, up = [], ["ninja compdb unreadable"]
        problems += up
        for drv in sorted({u["args"][0] for u in units}):
            ok, why = has_include_recorded(drv)
            if not ok:
                problems.append("files were added or deleted and %s: %s" % (drv, why))
        if "deleted" in kind.values() and not problems:
            new_src = {canon(os.path.join(root, f)) for f, k in kind.items() if k == "new"}
            try:
                with before_tree(kind, a.base):
                    before_reads, bp = read_sets_from_compiler([u for u in units if u["src"] not in new_src],
                                                               compiler_deps, a.jobs, canon)
            except RuntimeError as ex:
                return fail(str(ex))
            problems += bp
    tus, cov, dp = discover(bdir, root, kind, assumed, (deps.returncode, deps.stdout),
                            (compdb.returncode, compdb.stdout), edges, jobs=a.jobs, before_reads=before_reads)
    problems += dp
    names = {}
    try:
        names = receipt_names([t["obj"] for t in tus], bdir)
    except ValueError as ex:
        problems.append(str(ex))
    os.makedirs(a.out, exist_ok=True)
    for side in ("before", "after", "before_line0", "after_line0"):
        shutil.rmtree(os.path.join(a.out, side), ignore_errors=True)
    for side in ("before", "after"):
        os.makedirs(os.path.join(a.out, side))
    for f in os.listdir(a.out):
        if f.startswith("diff_") or f == "summary.tsv":
            os.remove(os.path.join(a.out, f))
    header = ["# pp_receipts: base %s, build dir %s" % (a.base, a.build_dir)] + ["# " + c for c in cov]
    for c in cov:
        print(c)
    if problems:
        with open(os.path.join(a.out, "summary.tsv"), "w", encoding="utf-8") as f:
            f.write("\n".join(header + ["# INCOMPLETE: " + p for p in problems]) + "\n")
        for p in problems[:50]:
            print("INCOMPLETE: " + p)
        return 2

    new_src = {canon(os.path.join(root, f)) for f, k in kind.items() if k == "new"}

    def path(side, t):
        return os.path.abspath(os.path.join(a.out, side, names[t["obj"]] + ".i.txt"))

    def pass_(side, units):
        """{obj: (error or None, read set or None)}, the outputs written to path(side, t)."""
        def one(t):
            ok, err, reads = preprocess(t["entry"], path(side, t), t["args"])
            return t["obj"], (err, reads)
        with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
            return dict(ex.map(one, units))

    olds = [t for t in tus if t["src"] not in new_src]
    after = pass_("after", tus)

    def markers(t):  # every unit's marker pass (-dE), on the AFTER tree: #embed and the PCH-probe excuse
        out = path("after", t)[:-6] + ".markers.txt"
        ent = entered_files(t["entry"], t["args"], out)
        embl = embed_lines(out) if ent is not None and shows_embeds(t["args"][0]) else None
        return t["obj"], (ent, embl)
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        marks = dict(ex.map(markers, olds))
    try:
        with before_tree(kind, a.base):
            before = pass_("before", olds)
    except RuntimeError as ex:
        return fail(str(ex))
    rows, ndiff, nfail = [], 0, 0
    for t in tus:
        t["read_set"] = "ninja deps (cross-checked)" if t.get("how") == "ninja deps" else t.get("how", "-")
        rel_src = os.path.relpath(t["src"], canon(root)).replace("\\", "/")
        obj = os.path.relpath(t["obj"], bdir).replace("\\", "/")
        if t["src"] in new_src:
            rows.append((rel_src, obj, "-", "-", "NEW", "no BEFORE unit"))
            ndiff += 1
            continue
        ent, embl = marks.get(t["obj"], (None, None))
        err = (before[t["obj"]][0] or after[t["obj"]][0] or marker_problem(ent, embl)
               or read_set_mismatch(t, after[t["obj"]][1], lambda t=t, ent=ent: unit_witness(t["entry"], t["args"],
                                                                                             ent)))
        if t.get("pch_explained"):
            t["read_set"] += ("; %d file(s) only its PCH's build record lists, each a clang builtin header's "
                              "__has_include_next probe (unentered, under a system include dir): %s" % (
                                  len(t["pch_explained"]), " ".join(t["pch_explained"])))
        if err:
            rows.append((rel_src, obj, "-", "-", "FAILED", str(err)[:400].replace("\n", " ")))
            nfail += 1
            continue
        try:
            xa, xb = read_text(path("before", t)), read_text(path("after", t))
            ta, tb = tokens(xa), tokens(xb)
        except LexError as ex:
            rows.append((rel_src, obj, "-", "-", "FAILED", "lexer: %s" % ex))
            nfail += 1
            continue
        if ta == tb:
            rows.append((rel_src, obj, str(len(ta)), str(len(tb)), "EQUAL", ""))
            continue
        ndiff += 1
        dl = difflib.unified_diff(display_lines(xa), display_lines(xb), "before/" + rel_src, "after/" + rel_src,
                                  lineterm="", n=2)
        with open(os.path.join(a.out, "diff_" + names[t["obj"]] + ".txt"), "w", encoding="utf-8",
                  errors="backslashreplace") as f:
            f.write("\n".join(dl) + "\n")
        rows.append((rel_src, obj, str(len(ta)), str(len(tb)), "DIFF", advisory(ta, tb)))
    with open(os.path.join(a.out, "summary.tsv"), "w", encoding="utf-8", errors="backslashreplace") as f:
        f.write("\n".join(header) + "\ntu\tobject\tbefore_tokens\tafter_tokens\tresult\tread_set\tnote\n")
        for r, t in zip(rows, tus):
            f.write("\t".join(r[:5] + (t["read_set"],) + r[5:]) + "\n")
    for r in rows:
        print("\t".join(r[:5]))
    print("%d affected units: %d EQUAL, %d DIFF / NEW, %d FAILED" % (len(rows), len(rows) - ndiff - nfail, ndiff,
                                                                     nfail))
    return 2 if nfail else 1 if ndiff else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir")
    ap.add_argument("--base", help="the BEFORE ref")
    ap.add_argument("--out")
    ap.add_argument("--assume-no-codegen", action="append", default=[], metavar="PATTERN=REASON")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--cc", help="--self-test: a clang++ / g++ for the compiler-backed checks")
    a = ap.parse_args()
    if a.self_test:
        return self_test(a.cc)
    if not (a.build_dir and a.base and a.out):
        ap.error("--build-dir, --base and --out are required (or --self-test)")
    try:
        return receipts(a)
    except Exception:  # an unexpected failure is INCOMPLETE (2), never Python's 1 (= DIFF)
        traceback.print_exc()
        print("pp_receipts: INCOMPLETE - an unexpected error")
        return 2


if __name__ == "__main__":
    sys.exit(main())
