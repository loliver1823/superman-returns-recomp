#!/usr/bin/env python3
"""test_gate_sweep.py - fixtures for tools/player/gate_sweep.py (the C1 removed-gate sweep, source side).

The sweep's SR_ORACLE views must read directives the way the preprocessor does, or a release body is silently dropped
or kept. This test proves, on synthetic sources and a synthetic tree:
  * comments in a directive are removed per translation phase 3, never by truncation: `#if SR_ORACLE /* c */ || 1`
    is an unmodelled SR_ORACLE conditional (exit 2), not an `#if SR_ORACLE` (Astra P3 on 610fac98); a comment that
    only decorates a split (`/* */`, `//`, one spanning lines) leaves it a split;
  * backslash-newline continuations are spliced per phase 2 before a directive is read: `#if SR_ORACLE \\ || 1` is
    unmodelled, `#if \\ SR_ORACLE` is a split, and a `//` comment ending in a backslash swallows the next line;
  * text that only looks like a directive is not one: inside a block comment, a raw string literal, or after a `/*`
    that sits in a string, a character literal or a `//` comment (the old comment stripper took `//...*0x8208/*...` as a
    block comment and hid 1,911 lines of kernel/stubs.cpp from the name scan);
  * every other SR_ORACLE form is exit 2 (`#ifdef`, `defined()`, parentheses, `== 1`, an #elif / #elifdef in or on a
    split, a second #else, unbalanced, an unterminated comment or raw string); the digraph `%:if` and blanks
    (`# if ! SR_ORACLE`) are read as the split they are; kernel/sr_oracle.hpp's default `#ifndef SR_ORACLE` is allowed;
  * names: string literals only (the lexer's own spans), comments excluded, removed vs surviving on a scratch tree,
    and the exit codes;
  * a fixture table, each row with what clang -E keeps under -DSR_ORACLE=0 / 1: the sweep must name exactly that, or
    exit 2 for a form it does not model - blanks are token boundaries (`#if SR_ORACLE and 1`, `not`, `defined`,
    `&&`), an alias (`#define ORA SR_ORACLE`), `#undef`, SR_ORACLE in code; a raw string's delimiter and body are read
    on the unspliced text (a splice inside one is reverted) and `"` is a delimiter character (R""(...)"");
  * the fail-closed rules of the last round (Astra round 3): a raw string literal in any directive, a string /
    character literal in a directive naming a macro an #if expression names (a __has_include("x") operand is a
    header-name, not a literal), and a non-ASCII character outside comments and literals are each exit 2 - with
    Astra's three repros (a raw string hiding SR_ORACLE in a #define's macro arguments, `"R"` followed by code, an
    identifier `eR` with a non-ASCII e before `"(`);
  * header-names are lexed where clang lexes them, before comments and literals (Astra round 4): an angle or quoted
    operand of __has_include - spelled out or through an alias like xxhash's XXH_HAS_INCLUDE - with `//`, `/*`, `"`,
    `'` or `\` inside no longer hides the rest of the #if; a quoted #if operand that reads differently as a string and
    as a header-name is exit 2;
  * ... but in an #if line `(<` is a header-name only as a plain path followed by `)` (Astra round 5: for an ordinary
    macro `#if F(< SR_ORACLE >)` is a macro argument) - any other `(<` operand there, and a header-name holding
    SR_ORACLE or a macro an #if names, is exit 2; xxhash's XXH_HAS_INCLUDE(<arm_neon.h>) form stays allowed;
  * the class closed (Astra round 6): an #if-family line may invoke (identifier then `(`) only an allowlisted form -
    `defined`, `__has_include`, and xxhash's XXH_HAS_* aliases, each verified to resolve to its builtin or a constant
    fallback by every definition in the files - so no macro argument can hide a token (`#if F(<X>)`, `#if F(X)` with
    X defined per SR_ORACLE); --calls lists the allowlist and every use.

  * the clang views' inputs, without a compiler (Astra round 8): literal VALUES in -E output - adjacent pieces
    concatenated across line markers and #pragma lines, escapes decoded, prefixes, raw pieces, character arrays and
    byte tables - and exit 2 on a lone quote or an escape that cannot be decoded; an #embed / .incbin reported; on a
    scratch tree with two build graphs and compile databases, the release is the exe's link closure: an empty
    release database, a missing SupermanReturnsGuest or sr_imgui entry, a release graph without the guest library or
    the exe, a linked ORACLE_SOURCES unit with no entry, an entry for another source and a prebuilt PCH are each
    exit 2; CMake's PCH reads as its text; a driver -include becomes cc1's; --uncovered-ok's format.
  * Astra round 8 (round 9 of the tool), without a compiler: a ud-suffix in non-ASCII or a universal-character-name
    keeps the literals around it concatenating; #pragma comment(lib / linker) strings are collected and one that is
    not a string is exit 2; a link rule whose own text adds an input - Astra's response-file repro (rspfile_content =
    $in extra.obj, extra.obj order-only), an object or a /defaultlib in the command, an unread @file, a program after
    the link, an edge's own `command`, an undefined rule - is exit 2; a forced include ahead of a PCH is exit 2. With
    --cc: the #pragma comment and --dependent-lib requests are UNCOVERED (a graph-built library the exe links is not,
    one it does not link is exit 2), and a PCH built in another context (Astra's -DSWITCH=1 repro, an include path
    that resolves a header elsewhere) is exit 2 while an extra include path that changes nothing passes.
  * Astra round 9 (round 10): a link command's tokens are the linker's - a quoted "extra.obj" in a response file
    (Astra's repro) or in the command is exit 2 -, a LINK_LIBRARIES bound by the file (Astra's repro) or the rule
    rather than the edge is exit 2, and so is a flag the tool does not know; with --cc, a PCH whose text depends on
    its place (__INCLUDE_LEVEL__) is exit 2.
  * Astra round 10 (round 11): the link commands come from ninja itself (the scratch graphs are ones ninja loads) -
    Astra's EXTRA repro (`EXTRA = extra.obj`, `LINK_LIBRARIES = $EXTRA`, `EXTRA =`: ninja keeps extra.obj) is exit 2,
    a linker edge's input spelled through a variable is one ninja reads differently from the closure (exit 2), and a
    response file of $in_newline passes; with --cc, the PCH comparison reads -E -P output as tokens (a raw string's
    `# 1 ...` line is content - Astra's repro is exit 2), and the EXTRA repro end to end: gate_sweep stops (exit 2)
    instead of handing exe_sweep a removed list without SR_HIDDEN for a binary that holds it.
  * Astra round 11 (round 12): a linker edge must be one of the tree's link command templates, filled with ninja's own
    values of its variables (never -x). Astra's three repros - the linker reading @exe.rsp.lib while ninja writes
    exe.rsp, a response file holding `&&cd hidden.obj`, a copy between two links - their CMake-template forms (a link
    flag naming @<rspfile>.more, `&&` in the response file, a static library's post-build copy over an object the exe
    links, a post-build step that links again), an edge's own rspfile / rspfile_content, a post-build step that is not
    a copy or copies over the exe, and a file-level PRE_LINK are each exit 2, acknowledged or not (the steps were
    UNCOVERED items); a quoted response-file argument ("@$RSP_FILE", @"$RSP_FILE") naming the edge's rspfile passes
    (Astra's P2).
  * Astra round 12 (round 13): a post-build copy naming the exe through a Windows path alias (Astra's \\\\?\\ repro,
    \\\\.\\, \\??\\, a UNC share, an 8.3 name, a trailing dot, an alternate data stream), a link argument through one, and
    a copy through a directory junction or a hard link (compared by resolved identity) are exit 2; so are a UTF-8 BOM
    before the response file's first input (Astra's repro: clang drops it) and a non-ASCII input name, and the
    command line still exits 2 when its message quotes one; with --cc, both repros end to end through exe_sweep.
  * with --cc, the authority itself (Astra round 7: the lexer emulates the preprocessor and keeps leaking, so the two
    views are clang's) on that tree: clang decides Astra's two repros (`#define CALL F(` + `#if CALL X)`;
    `#define CALL F(X)` + `#if CALL`), reads a header through the unit that includes it, drops an ORACLE_SOURCES unit
    the release graph does not link, lists a release and a difftest survivor and an unreachable header; round 8:
    every literal value (each confirmed in the object clang compiles) is removed, sr_imgui's reader survives, the
    PCH's header is read, and the resource script, the DLL, the system library and the #embed are UNCOVERED - exit 2
    until acknowledged (--uncovered-ok), a stale acknowledgement reported; exit 2 when a unit fails to preprocess,
    holds a literal that cannot be decoded, or the compiler cannot run; the lexer is only compared.

    python tools/player/test_gate_sweep.py [--cc <clang++>]   # exit 0 = every check passed; 1 = some failed (named)

--cc re-confirms every fixture's clang column on that compiler (run it on the cloud worker:
`python tools/cloud/sr_cloud.py run --name <tree> -- python tools/player/test_gate_sweep.py --cc clang++`).
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)        # a plain import: the clang views' worker processes find gate_sweep by name
import gate_sweep as gs  # noqa: E402

FAILS = []


def check(name, cond, detail=''):
    print('%s %s%s' % ('ok  ' if cond else 'FAIL', name, (' - ' + str(detail)[:500]) if detail and not cond else ''))
    if not cond:
        FAILS.append(name)


def views(text, name='t.cpp'):
    """(view0, view1) or the Unmodelled message."""
    try:
        return gs.view(text, 0, name), gs.view(text, 1, name)
    except gs.Unmodelled as e:
        return str(e)


def split_ok(name, text, rel_line, dev_line):
    """`text` is read as a split: the release view keeps rel_line and drops dev_line, the dev view the opposite."""
    r = views(text)
    ok = isinstance(r, tuple) and rel_line in r[0] and dev_line not in r[0] and dev_line in r[1] \
        and rel_line not in r[1]
    check(name, ok, r)


def unit_checks():
    D, R = 'const char* d = "SR_DEV";', 'const char* r = "SR_REL";'
    plain = f'#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n'
    split_ok('baseline: #if SR_ORACLE / #else / #endif is a split', plain, R, D)

    # --- comments (Astra P3: the directive was truncated at /*) ---
    r = views(f'#if SR_ORACLE /* comment */ || 1\n{D}\n#else\n{R}\n#endif\n')
    check('`#if SR_ORACLE /* c */ || 1` is unmodelled (exit 2), not a split', isinstance(r, str) and 'unmodelled' in r, r)
    r = views(f'#if SR_ORACLE // comment\n{D}\n#endif\n')
    check('a trailing // comment leaves a split', isinstance(r, tuple) and D not in r[0] and D in r[1], r)
    split_ok('a /* */ comment inside the directive leaves a split', f'#if /* x */ SR_ORACLE /* y */\n{D}\n#else /* z */\n'
             f'{R}\n#endif /* w */\n', R, D)
    r = views(f'#if SR_ORACLE /* spans\n   lines */ || 1\n{D}\n#endif\n')
    check('a comment spanning lines inside the directive is read through: `|| 1` after it is unmodelled',
          isinstance(r, str) and 'unmodelled' in r, r)
    split_ok('a multi-line comment after `#if SR_ORACLE` leaves a split, its lines go with the directive',
             f'#if SR_ORACLE /* one\n two */\n{D}\n#else\n{R}\n#endif\n', R, D)
    r = views(f'#if !SR_ORACLE /* c */ && X\n{R}\n#endif\n')
    check('`#if !SR_ORACLE /* c */ && X` is unmodelled', isinstance(r, str) and 'unmodelled' in r, r)

    # --- continuations (phase 2) ---
    r = views(f'#if SR_ORACLE \\\n  || 1\n{D}\n#endif\n')
    check('`#if SR_ORACLE \\<newline> || 1` is unmodelled', isinstance(r, str) and 'unmodelled' in r, r)
    r = views(f'#if SR_ORACLE \\   \n  || 1\n{D}\n#endif\n')
    check('a continuation with blanks after the backslash is spliced too (clang / gcc do)',
          isinstance(r, str) and 'unmodelled' in r, r)
    r = views(f'#if X \\\n  || !SR_ORACLE\n{D}\n#else\n{R}\n#endif\n')
    check('`#if X \\<newline> || !SR_ORACLE` is unmodelled (the old reader kept both branches: a hidden split)',
          isinstance(r, str) and 'unmodelled' in r, r)
    split_ok('`#if \\<newline> SR_ORACLE` is a split', f'#if \\\n SR_ORACLE\n{D}\n#else\n{R}\n#endif\n', R, D)
    r = views(f'#if SR_ORACLE\n{D}\n#else\n{R}\n#endif \\\n && 1\n')
    check('a continued #endif keeps the line it swallows out of both views (one directive)',
          isinstance(r, tuple) and '&& 1' not in r[0] + r[1], r)
    t = f'#if SR_ORACLE\n{D}\n// a note \\\n#endif\n#endif\n'
    r = views(t)
    check('a // comment ending in a backslash swallows the next line: that #endif is comment, the next one closes',
          isinstance(r, tuple) and D not in r[0] and D in r[1], r)
    r = views(f'#if SR_ORACLE\n{D}\n// a note \\\n#endif\n')
    check('... and without the second #endif the split is unbalanced (exit 2)',
          isinstance(r, str) and 'unbalanced' in r, r)
    r = views(f'const char* s = "a\\\nSR_X";\n')
    check('a continued line is one logical line in the view output (both physical lines kept)',
          isinstance(r, tuple) and r[0] == r[1] == 'const char* s = "a\\\nSR_X";\n', r)

    # --- look-alikes that are not directives ---
    split_ok('`#if SR_ORACLE` inside a block comment is not a directive',
             f'/*\n#if SR_ORACLE\n*/\n#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n', R, D)
    split_ok('`#if SR_ORACLE` inside a raw string literal is not a directive',
             f'const char* raw = R"x(\n#if SR_ORACLE\n)x";\n#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n', R, D)
    split_ok('a raw string with `/*` and `"` inside does not open a comment or a string',
             f'const char* raw = u8R"(/* " \')";\n#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n', R, D)
    split_ok('`/*` in a string does not open a comment', f'const char* s = "/*";\n#if SR_ORACLE\n{D}\n#else\n{R}\n'
             f'#endif\nconst char* e = "*/";\n', R, D)
    split_ok("`'\"'` (a quote character literal) does not open a string",
             f"char q = '\"'; const char* c = \"/*\";\n#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n", R, D)
    split_ok('a digit separator does not open a character literal',
             f"int n = 1'000'000; char c = '/';\n#if SR_ORACLE\n{D}\n#else\n{R}\n#endif\n", R, D)
    t = '// static+gold (*0x8208A864/*0x82074190, see below)\nconst char* k = "SR_AFTER";\n/* a real one */\n'
    check('`/*` inside a // comment does not open a block comment (names after it are seen)',
          gs.view_names(t, 't.cpp')[0] == {'SR_AFTER'}, gs.view_names(t, 't.cpp'))

    # --- other SR_ORACLE forms ---
    for label, d in (('#ifdef SR_ORACLE', '#ifdef SR_ORACLE'), ('#ifndef SR_ORACLE (outside sr_oracle.hpp)',
                     '#ifndef SR_ORACLE'), ('#if defined(SR_ORACLE)', '#if defined(SR_ORACLE)'),
                     ('#if (SR_ORACLE)', '#if (SR_ORACLE)'), ('#if SR_ORACLE == 1', '#if SR_ORACLE == 1'),
                     ('#if SR_ORACLE && X', '#if SR_ORACLE && X')):
        r = views(f'{d}\n{D}\n#endif\n')
        check(f'`{label}` is unmodelled (exit 2)', isinstance(r, str) and 'unmodelled' in r, r)
    r = views(f'#ifndef SR_ORACLE\n#define SR_ORACLE 1\n#endif\n', 'kernel/sr_oracle.hpp')
    check("kernel/sr_oracle.hpp's default definition is allowed", isinstance(r, tuple), r)
    r = views(f'#if SR_ORACLE\n{D}\n#elif X\n{R}\n#endif\n')
    check('#elif in a split is exit 2', isinstance(r, str) and '#elif' in r, r)
    r = views(f'#if SR_ORACLE\n{D}\n#elifdef X\n{R}\n#endif\n')
    check('#elifdef in a split is exit 2', isinstance(r, str) and '#elifdef' in r, r)
    r = views(f'#if X\n{D}\n#elif SR_ORACLE\n{R}\n#endif\n')
    check('#elif naming SR_ORACLE is exit 2', isinstance(r, str) and '#elif' in r, r)
    r = views(f'#if SR_ORACLE\n{D}\n#else\n{R}\n#else\n#endif\n')
    check('a second #else in a split is exit 2', isinstance(r, str) and 'second #else' in r, r)
    r = views(f'#if SR_ORACLE\n{D}\n')
    check('an unbalanced split is exit 2', isinstance(r, str) and 'unbalanced' in r, r)
    r = views(f'/* never closed\n#if SR_ORACLE\n')
    check('an unterminated block comment is exit 2', isinstance(r, str) and 'unterminated' in r, r)
    r = views(f'const char* r = R"x(never closed\n')
    check('an unterminated raw string is exit 2', isinstance(r, str) and 'unterminated' in r, r)
    split_ok('the digraph `%:if SR_ORACLE` is a split', f'%:if SR_ORACLE\n{D}\n%:else\n{R}\n%:endif\n', R, D)
    split_ok('`# if ! SR_ORACLE` (blanks) is the !SR_ORACLE split', f'# if ! SR_ORACLE\n{R}\n#  else\n{D}\n# endif\n',
             R, D)
    split_ok('an unrelated #if / #elif inside a split is kept whole',
             f'#if SR_ORACLE\n#if A\n{D}\n#elif B\n#endif\n#else\n{R}\n#endif\n', R, D)

    # --- names ---
    t = f'// "SR_COMMENT"\n/* "SR_BLOCK" */\n{plain}const char* x = L"SR_WIDE";\n'
    n = gs.view_names(t, 't.cpp')
    check('names: literals only, comments excluded, per view',
          n == {1: {'SR_DEV', 'SR_WIDE'}, 0: {'SR_REL', 'SR_WIDE'}}, n)


BS = chr(92)
DEV, REL, ALL = 'const char* d = "SR_DEV";', 'const char* r = "SR_REL";', 'const char* a = "SR_ALL";'
SPLIT = f'#if SR_ORACLE\n{DEV}\n#else\n{REL}\n#endif\n'
# (label, source, what clang -E -P -DSR_ORACLE=<v> keeps {v: the SR_* runs in its literals} (None: not a preprocessor
#  question; a raw string's body is a literal: `#if SR_ORACLE` written inside one names SR_ORACLE),
#  'model' = the sweep's views must name exactly what clang keeps, 'reject' = the sweep must exit 2 (unmodelled)).
# `python tools/player/test_gate_sweep.py --cc <clang++>` re-confirms every clang column on the real preprocessor.
FIXTURES = [
    # Astra round 2, P2: blanks are token boundaries - the reader must see `SR_ORACLE and 1`, not `SR_ORACLEand1`.
    ('`#if SR_ORACLE and 1` (alternative token)', f'#if SR_ORACLE and 1\n{DEV}\n#endif\n{ALL}\n',
     {0: {'SR_ALL'}, 1: {'SR_DEV', 'SR_ALL'}}, 'reject'),
    ('`#if SR_ORACLE&&1` (no blanks)', f'#if SR_ORACLE&&1\n{DEV}\n#endif\n{ALL}\n',
     {0: {'SR_ALL'}, 1: {'SR_DEV', 'SR_ALL'}}, 'reject'),
    ('`#if X and SR_ORACLE`', f'#define X 1\n#if X and SR_ORACLE\n{DEV}\n#endif\n',
     {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if not SR_ORACLE`', f'#if not SR_ORACLE\n{REL}\n#endif\n', {0: {'SR_REL'}, 1: set()}, 'reject'),
    ('`#if SR_ORACLE or 0`', f'#if SR_ORACLE or 0\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if defined SR_ORACLE`', f'#if defined SR_ORACLE\n{DEV}\n#endif\n', {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('`#if defined(SR_ORACLE)`', f'#if defined(SR_ORACLE)\n{DEV}\n#endif\n', {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('`#if SR_ORACLE /* c */ || 1`', f'#if SR_ORACLE /* c */ || 1\n{DEV}\n#endif\n',
     {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    (f'continued `#if X {BS} || !SR_ORACLE`', f'#define X 0\n#if X {BS}\n || !SR_ORACLE\n{REL}\n#endif\n',
     {0: {'SR_REL'}, 1: set()}, 'reject'),
    ('`#define ORA SR_ORACLE` then `#if ORA` (an alias)', f'#define ORA SR_ORACLE\n#if ORA\n{DEV}\n#endif\n',
     {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#undef SR_ORACLE`', f'#undef SR_ORACLE\n#if SR_ORACLE\n{DEV}\n#endif\n', {0: set(), 1: set()}, 'reject'),
    ('`if constexpr (SR_ORACLE)` in code', f'void f() {{ if constexpr (SR_ORACLE) {{ {DEV} }} }}\n', None, 'reject'),
    # what must stay a split
    ('`#if ! SR_ORACLE` (blanks)', f'#if ! SR_ORACLE\n{REL}\n#else\n{DEV}\n#endif\n',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('`#if/* c */SR_ORACLE//c`', f'#if/* c */SR_ORACLE//c\n{DEV}\n#else\n{REL}\n#endif\n',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    (f'`#if {BS}` + `SR_ORACLE` (continued)', f'#if {BS}\nSR_ORACLE\n{DEV}\n#else\n{REL}\n#endif\n',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('`%:if SR_ORACLE` (digraph)', f'%:if SR_ORACLE\n{DEV}\n%:else\n{REL}\n%:endif\n',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('a "SR_ORACLE" string literal is not the identifier', f'const char* n = "SR_ORACLE";\n{SPLIT}',
     {0: {'SR_ORACLE', 'SR_REL'}, 1: {'SR_ORACLE', 'SR_DEV'}}, 'model'),
    # Astra round 2, P3: a raw string's delimiter and body are read on the unspliced text.
    (f'a splice inside a raw string is reverted: `)x{BS}`newline`y"` does not close R"xy(',
     f'const char* r = R"xy(abc)x{BS}\ny";\n#if SR_ORACLE\n)xy";\n{SPLIT}',
     {0: {'SR_ORACLE', 'SR_REL'}, 1: {'SR_ORACLE', 'SR_DEV'}}, 'model'),
    (f'... and `){BS}`newline`x"` neither', f'const char* r = R"x(abc){BS}\nx";\n#if SR_ORACLE\n)x";\n{SPLIT}',
     {0: {'SR_ORACLE', 'SR_REL'}, 1: {'SR_ORACLE', 'SR_DEV'}}, 'model'),
    # Astra round 2, P3: `"` is a valid raw-string delimiter character.
    ('R""( ... )"" (the delimiter `"`) is one raw string', f'const char* q = R""(\n#if SR_ORACLE\n)"";\n{SPLIT}',
     {0: {'SR_ORACLE', 'SR_REL'}, 1: {'SR_ORACLE', 'SR_DEV'}}, 'model'),
    ('u8R"--( ... )--" spanning a directive look-alike', f'auto q = u8R"--(\n#if SR_ORACLE\n)--";\n{SPLIT}',
     {0: {'SR_ORACLE', 'SR_REL'}, 1: {'SR_ORACLE', 'SR_DEV'}}, 'model'),
    ('a `#if SR_ORACLE` inside a block comment is not a directive', f'/*\n#if SR_ORACLE\n*/\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    (f'a // comment continued by a backslash swallows `#endif`', f'#if SR_ORACLE\n{DEV}\n// x {BS}\n#endif\n#endif\n',
     {0: set(), 1: {'SR_DEV'}}, 'model'),
    # Astra round 3 (last round): fail closed broadly. Its three repros, each compiled by clang in both configurations.
    ('a raw string in a #define\'s macro arguments hiding SR_ORACLE (Astra repro 1)',
     '#define MID(a,b,c) b\n#define ORA MID(R"x(")x", SR_ORACLE, R"x(")x")\n#if ORA\nconst char*d="SR_DEV";\n#endif\n',
     {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`"R"` then code naming SR_ORACLE then `");if"` (Astra repro 2)',
     'void f(){const char*s="R";if(SR_ORACLE){const char*d="SR_DEV";}const char*e=");if";}\n',
     {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('an identifier with a non-ASCII letter before `R"(` (Astra repro 3)',
     '#define \u00e9R\nconst char*s=\u00e9R"("\n#if SR_ORACLE\nconst char*d="SR_DEV";\n#endif\n")";\n',
     {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('a raw string literal in any directive', f'#define RAWS R"(x)"\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    ('a string literal in a #define of a macro an #if names', f'#define TAG "x"\n#if defined(TAG)\n{DEV}\n#endif\n',
     {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('a character literal in a #define of a macro an #if names', f"#define CH 'x'\n#if CH == 120\n{DEV}\n#endif\n",
     {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('a non-ASCII character in code', f'int \u00e9 = 0;\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    ('a __has_include("x") operand is a header-name, not a literal',
     f'#if __has_include("no_such_header.h")\nint x;\n#endif\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('a string in a #define of a macro no #if names stays allowed', f'#define LOGTAG "x"\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('non-ASCII inside a comment and a string literal stays allowed',
     f'// \u00e9\nconst char* u = "\u00e9";\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('a leading UTF-8 byte-order mark stays allowed', f'\ufeff{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    # Astra round 4: header-names are lexed before comments - an operand's `//` / `/*` / quote no longer hides
    # SR_ORACLE (which then makes the #if unmodelled, exit 2); clean operands stay allowed.
    ('`#if __has_include(<no//such>) || SR_ORACLE` (Astra repro)',
     f'#if __has_include(<no//such>) || SR_ORACLE\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(<no/*such>) || SR_ORACLE // */` (Astra repro, block-comment variant)',
     f'#if __has_include(<no/*such>) || SR_ORACLE // */\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(<no"such>) || SR_ORACLE` (a quote in an angle header-name)',
     f'#if __has_include(<no"such>) || SR_ORACLE\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include("no\\") || SR_ORACLE` (header-name and string readings differ; clang errors: '
     'unterminated string)', f'#if __has_include("no\\") || SR_ORACLE\n{DEV}\n#endif\n', None, 'reject'),
    ('`#if HI(<no//such>) || SR_ORACLE` through `#define HI __has_include` (an alias)',
     f'#define HI __has_include\n#if HI(<no//such>) || SR_ORACLE\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}},
     'reject'),
    ('`#if F("a\\"b")` - a quoted #if operand read differently as a string and a header-name',
     f'#define F(x) 1\n#if F("a\\"b")\n{DEV}\n#endif\n', {0: {'SR_DEV'}, 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(<no_such.h>)` (a clean angle operand) stays allowed',
     f'#if __has_include(<no_such.h>)\nint x;\n#endif\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    ('an alias the allowlist does not hold, `#if HI(<no_such.h>)` (HI = __has_include)',
     f'#define HI __has_include\n#if defined(X) && HI(<no_such.h>)\nint y;\n#endif\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    # Astra round 5: `(<...>` in an #if line is a header-name only as a plain path followed by `)`.
    ('`#define F(x) (0 x 0)` + `#if F(< SR_ORACLE >)` - a macro argument, not a header-name (Astra repro)',
     f'#define F(x) (0 x 0)\n#if F(< SR_ORACLE >)\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(< no_such.h >)` (a spaced header-name)',
     f'#if __has_include(< no_such.h >)\nint x;\n#endif\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    ('`#if G(<SR_ORACLE>)` - a plain path that holds SR_ORACLE (`(0 <SR_ORACLE> 0)` for an ordinary G)',
     f'#define G(x) (0 x 0)\n#if G(<SR_ORACLE>)\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(<FOO.h>)` with FOO a macro an #if names',
     f'#define FOO 1\n#if FOO\nint a;\n#endif\n#if __has_include(<FOO.h>)\nint b;\n#endif\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    ('`#if __has_include(<no_such.h> /* c */)` - the `)` must follow the header-name',
     f'#if __has_include(<no_such.h> /* c */)\nint x;\n#endif\n{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'reject'),
    ('xxhash\'s own form `XXH_HAS_INCLUDE(<arm_neon.h>)` through its alias stays allowed',
     f'#define XXH_HAS_INCLUDE __has_include\n#if defined(X) && XXH_HAS_INCLUDE(<arm_neon.h>)\nint y;\n#endif\n'
     f'{SPLIT}', {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
    # Astra round 6: close the class - only allowlisted function-like forms in an #if line.
    ('`#if F(<X>)` with X a character literal per SR_ORACLE (Astra repro)',
     "#define F(x) (0 x 0)\n#if SR_ORACLE\n#define X 'x'\n#else\n#define X '\\0'\n#endif\n#if F(<X>)\n"
     f"{DEV}\n#endif\n", {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('`#if F(X)` with X defined per SR_ORACLE (no header shape)',
     f'#define F(x) (x)\n#if SR_ORACLE\n#define X 1\n#else\n#define X 0\n#endif\n#if F(X)\n{DEV}\n#endif\n',
     {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ('an allowlisted alias name whose definition passes its argument through, `XXH_HAS_INCLUDE(x) (x)`',
     f'#define XXH_HAS_INCLUDE(x) (x)\n#if SR_ORACLE\n#define X 1\n#else\n#define X 0\n#endif\n'
     f'#if XXH_HAS_INCLUDE(X)\n{DEV}\n#endif\n', {0: set(), 1: {'SR_DEV'}}, 'reject'),
    ("xxhash's two-definition form (`XXH_HAS_BUILTIN(x) __has_builtin(x)` / `0`) stays allowed",
     f'#if defined(__has_builtin)\n#define XXH_HAS_BUILTIN(x) __has_builtin(x)\n#else\n#define XXH_HAS_BUILTIN(x) 0\n'
     f'#endif\n#if XXH_HAS_BUILTIN(__builtin_unreachable)\nint z;\n#endif\n{SPLIT}',
     {0: {'SR_REL'}, 1: {'SR_DEV'}}, 'model'),
]


def fixture_checks():
    for label, text, truth, expect in FIXTURES:
        try:
            got = gs.view_names(text, 't.cpp')
        except gs.Unmodelled as e:
            got = 'exit 2: %s' % e
        if expect == 'reject':
            check('rejects %s (exit 2)' % label, isinstance(got, str), got)
        else:
            check('reads %s as clang does' % label, got == truth, '%s; clang: %s' % (got, truth))


def clang_checks(cc, base):
    """Each fixture's clang column, on the real preprocessor (C++20, -DSR_ORACLE=0 / 1, as the build defines it)."""
    for k, (label, text, truth, _) in enumerate(FIXTURES):
        if truth is None:
            continue
        path = os.path.join(base, 'fx%02d.cpp' % k)
        with open(path, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        seen, err = {}, ''
        for v in (0, 1):
            p = subprocess.run([cc, '-E', '-P', '-x', 'c++', '-std=c++20', '-DSR_ORACLE=%d' % v, path],
                               capture_output=True, text=True)
            if p.returncode:
                err = p.stderr.strip()[:300]
            try:      # the clang views' own reader of literal values: the same names the sweep counts
                seen[v] = set().union(*gs.output_names(p.stdout, base, base)[0].values())
            except gs.Unmodelled as e:
                err = 'the -E output cannot be read: %s' % e
        check('clang confirms: %s' % label, not err and seen == truth, err or '%s; table: %s' % (seen, truth))


LIT_NAMES = ('SR_LIVE1', 'SR_LIVE2', 'SR_LIVE3', 'SR_LIVE4', 'SR_LIVE5', 'SR_LIVE6', 'SR_LIVE7', 'SR_ARR', 'SR_BYTE',
             'SR_LIVE8', 'SR_LIVE9')
# Astra round 8, P1-a: clang emits each of these values (the --cc run compiles the unit and finds NAME\0 in the object),
# whatever its spelling: adjacent literals, a stringized argument, hex / octal / UCN / named escapes, a u8 prefix, a
# raw piece, a character array, a byte table; round 9: a user-defined literal whose suffix is non-ASCII (_é), or a
# universal-character-name (_\u00e9), still concatenates.
LIT_CPP = r'''#define S(x) "SR_" #x
constexpr const char* operator""_é(const char* s, decltype(sizeof 0)) { return s; }
#if SR_ORACLE
extern const char* const l1 = "SR_" "LIVE1";
extern const char* const l2 = S(LIVE2);
extern const char* const l3 = "SR_\x4cIVE3";
extern const char8_t* const l4 = u8"SR_\u004cIVE4";
extern const char* const l5 = "SR_\114IVE5";
extern const char* const l6 = R"(SR_)" "LIVE6";
extern const char* const l7 = "SR_\N{LATIN CAPITAL LETTER L}IVE7";
extern const char arr[] = {'S', 'R', '_', 'A', 'R', 'R', 0};
extern const unsigned char byt[] = {0x53, 0x52, 0x5f, 0x42, 0x59, 0x54, 0x45, 0};
extern const char* const l8 = "SR_"_é "LIVE8";
extern const char* const l9 = "SR_"_\u00e9 "LIVE9";
#endif
'''


def scan_checks():
    """Literal VALUES in clang's -E output (Astra round 8, P1-a), on synthetic -E text - no compiler needed."""
    root = os.path.abspath(os.sep)
    text = ('# 1 "p.cpp"\n'
            'const char* a = "SR_" "LIVE1";\n'
            'const char* b = "SR_" "LIVE2";\n'                     # what S(LIVE2) becomes after -E
            'const char* c = "SR_\\x4cIVE3";\n'
            'const char* d = u8"SR_\\u004c" L"IVE4";\n'
            'const char* e = "SR_\\114IVE5";\n'
            'const char* f = R"x(SR_)x" "LIVE6";\n'
            'const char* g = "SR_\\N{LATIN CAPITAL LETTER L}IVE7";\n'
            "extern const char arr[] = {'S', 'R', '_', 'A', 'R', 'R', 0};\n"
            'extern const unsigned char byt[] = {0x53, 0x52, 0x5f, 0x42, 0x59, 0x54, 0x45, 0};\n'
            'const char* h = "SR_"_\u00e9 "LIVE8";\n'                 # -E keeps the suffix as written: UTF-8 ...
            'const char* i = "SR_"_\\u00e9 "LIVE9";\n')               # ... or a universal-character-name
    names, _ = gs.output_names(text, root, root)
    got = names.get('p.cpp', set())
    check('P1-a: a literal\'s value is read - adjacent pieces concatenated, escapes decoded (hex, octal, \\u, \\N{}), '
          'prefixes and raw pieces, character arrays and byte tables', got >= set(LIT_NAMES[:9]), sorted(got))
    check('round 9: a user-defined literal suffix in non-ASCII (_\u00e9) or a universal-character-name (_\\u00e9) is '
          'one identifier, so the literals around it still concatenate', {'SR_LIVE8', 'SR_LIVE9'} <= got, sorted(got))
    names, _ = gs.output_names('# 1 "p.cpp"\nconst char* x = "x"\n# 7 "h.hpp" 1\n"SR_ACROSS"\n# 2 "p.cpp" 2\n;\n'
                               'const char* y = "a"\n#pragma pack(push, 8)\n"SR_AFTER_PRAGMA";\n'
                               '#pragma message("SR_IN_PRAGMA")\nconst char* z = "log: SR_MID=%d";\n', root, root)
    check('P1-a: concatenation runs across line markers and #pragma lines, is attributed to its first piece\'s file, '
          'and a name anywhere in the value counts (log text, a #pragma\'s own literal)',
          names == {'p.cpp': {'SR_ACROSS', 'SR_AFTER_PRAGMA', 'SR_IN_PRAGMA', 'SR_MID'}}, names)
    for label, body in (('a lone `"` (an unterminated string)', 'const char* s = "abc;\n'),
                        ("a lone `'`", "int x = 'a;\n"),
                        ('`\\x{zz}`', 'const char* s = "\\x{zz}";\n'),
                        ('`\\N{BOGUS NAME}`', 'const char* s = "\\N{BOGUS NAME}";\n'),
                        ('`\\u12` (short of digits)', 'const char* s = "\\u12";\n'),
                        ('`\\x` with no digit', 'const char* s = "\\xg";\n'),
                        ('a raw string the output does not close', 'const char* s = R"x(abc";\n'),
                        ('a #pragma comment(lib) naming no string (a macro clang left)',
                         '#pragma comment(lib, LIBNAME)\n')):
        try:
            r = gs.output_names('# 1 "p.cpp"\n' + body, root, root)
        except gs.Unmodelled as e:
            r = 'exit 2: %s' % e
        check('P1-a: exit 2 on a literal that cannot be decoded: %s' % label, isinstance(r, str), r)
    dec = getattr(gs, 'decode_escapes', None)
    table = ((r'\n\t\\\"\'\?', '\n\t\\"\'?'), (r'\101\0', 'A\0'), (r'\x41\x{42}', 'AB'), (r'\o{103}', 'C'),
             (r'\u0044\U00000045\u{46}', 'DEF'), (r'\N{LATIN SMALL LETTER G}', 'g'), (r'\%', '%'), (r'\e', '\x1b'),
             (r'A\x9999999aG', 'A\ufffdG'))   # fmt's U"..." tables: a char32_t code unit past U+10FFFF, no name char
    for spelled, value in table:
        got = dec(spelled) if dec else None
        check('P1-a: the escape %s decodes as clang does' % spelled, got == value, ascii(got))
    if hasattr(gs, 'output_scan'):
        names, _, inputs = gs.output_scan(
            '# 1 "e.cpp"\nconst unsigned char b[] = {\n#embed "blob.bin" /* clang -E -dE */\n};\n'
            'asm(".incbin \\"x.bin\\"");\n#pragma comment(lib, "cfg" "mgr32.lib")\n#pragma comment( lib , "psapi" )\n'
            '#pragma comment(linker, "/DEFAULTLIB:x.lib")\n#pragma comment(user, "SR_IN_USER")\n', root, root)
        check('P2-b: an #embed (clang -dE prints the directive, not its bytes) and an .incbin string are reported',
              inputs[:2] == [('e.cpp', 'embed', '#embed "blob.bin"'),
                             ('e.cpp', 'incbin', '.incbin (a string literal holding an assembler embed)')], inputs)
        check('round 9: #pragma comment(lib) and comment(linker) - the object\'s own requests of the linker - are '
              'collected (the string\'s value), a comment(user) string is read for names',
              inputs[2:] == [('e.cpp', 'lib', 'cfgmgr32.lib'), ('e.cpp', 'lib', 'psapi'),
                             ('e.cpp', 'linker', '/DEFAULTLIB:x.lib')] and names == {'e.cpp': {'SR_IN_USER'}},
              (inputs, names))
    else:
        check('P2-b: an #embed and an .incbin string are reported', False, 'no output_scan')
        check('round 9: #pragma comment(lib) and comment(linker) are collected', False, 'no output_scan')


def nj(path):
    """A path as build.ninja spells it."""
    return path.replace('\\', '/').replace('$', '$$').replace(':', '$:').replace(' ', '$ ')


LD, AR, RANLIB = 'C:/llvm/bin/clang++.exe', 'C:/llvm/bin/llvm-ar.exe', 'C:/llvm/bin/llvm-ranlib.exe'
CMAKE = '"C:\\Program Files\\CMake\\bin\\cmake.exe"'
CMD = 'C:\\WINDOWS\\system32\\cmd.exe /C '
# CMake's link rules for clang on Windows (rules.ninja), the executable's and one static library's through a response
# file as CMake writes them for a long link line.
LINK_RULES = {
    'CXX_EXECUTABLE_LINKER__SupermanReturns_Release': {
        'command': CMD + '"$PRE_LINK && ' + LD + ' -nostartfiles -nostdlib $FLAGS $LINK_FLAGS @$RSP_FILE -o $TARGET_FILE '
                   '-Xlinker /MANIFEST:EMBED -Xlinker /implib:$TARGET_IMPLIB -Xlinker /pdb:$TARGET_PDB -Xlinker '
                   '/version:0.0  $MANIFESTS && $POST_BUILD"',
        'rspfile': '$RSP_FILE', 'rspfile_content': '$in $LINK_PATH $LINK_LIBRARIES'},
    'CXX_STATIC_LIBRARY_LINKER__SupermanReturnsGuest_Release': {
        'command': CMD + '"$PRE_LINK && ' + CMAKE + ' -E rm -f $TARGET_FILE && ' + AR + ' qc $TARGET_FILE $LINK_FLAGS '
                   '@$RSP_FILE && ' + RANLIB + ' $TARGET_FILE && $POST_BUILD"',
        'rspfile': '$RSP_FILE', 'rspfile_content': '$in $LINK_PATH $LINK_LIBRARIES'},
    'CXX_STATIC_LIBRARY_LINKER__sr_imgui_Release': {
        'command': CMD + '"$PRE_LINK && ' + CMAKE + ' -E rm -f $TARGET_FILE && ' + AR + ' qc $TARGET_FILE $LINK_FLAGS '
                   '$in && ' + RANLIB + ' $TARGET_FILE && $POST_BUILD"'},
}


class Tree:
    """A scratch tree with two CMake-like builds (bd: SR_ORACLE=1, bd_rel: SR_RELEASE_COMPOSITION=ON), each with a
    build.ninja (the link closure of SupermanReturns.exe, CMake's link rules in an included rules.ninja) and a compile
    database: the exe's own units, the SupermanReturnsGuest and sr_imgui static libraries, a resource script, a DLL it
    links through its import library, a system library, a PCH, #pragma comment and --dependent-lib requests, a
    post-build copy, and a difftest unit outside the closure."""
    EXE = ['kernel/a.cpp', 'kernel/b.cpp', 'kernel/c.cpp', 'kernel/o.cpp', 'kernel/lit.cpp', 'kernel/g.cpp',
           'kernel/e.cpp', 'kernel/pch_user.cpp']
    C_CPP = ('const char* c = "SR_ORA";  // a release reader of the oracle unit\'s gate\n'
             # round 9: an object's own requests of the linker - a library, a linker directive (through _Pragma) - and
             # one for a library the graph builds and the exe links (no item: its units are in the views)
             '#pragma comment(lib, "cfgmgr32.lib")\n_Pragma("comment(linker, \\"/DEFAULTLIB:extra.lib\\")")\n'
             '#pragma comment(lib, "sr_imgui.lib")\n')

    def __init__(self, base, cc):
        self.cc, self.root = cc, os.path.join(base, 'clang_tree')
        self.bd, self.bd_rel = os.path.join(self.root, 'bd'), os.path.join(self.root, 'bd_rel')
        for d in ('kernel', 'tools/difftest', 'ext', 'app', 'pinc', 'pinc2', 'bd/pch', 'bd_rel/pch'):
            os.makedirs(os.path.join(self.root, d), exist_ok=True)
        put = self.put
        put('CMakeLists.txt', 'set(APP_SOURCES\n  kernel/a.cpp\n  kernel/b.cpp\n  kernel/c.cpp\n  kernel/o.cpp\n)\n'
                              'set(ORACLE_SOURCES\n  kernel/o.cpp\n)\n')
        # Astra round 7, repro 1: an object-like macro supplies the call and its `(`.
        put('kernel/a.cpp', '#include "h.hpp"\n#define F(x) (x)\n#define CALL F(\n#if SR_ORACLE\n#define X 1\n#else\n'
                            '#define X 0\n#endif\n#if CALL X)\nconst char* d = "SR_DEV";\n#endif\n'
                            'const char* k = "SR_KEPT";\n')
        # Astra round 7, repro 2: `#define CALL F(X)` with the round-6 character-literal X.
        put('kernel/b.cpp', "#define F(x) (0 < x > 0)\n#if SR_ORACLE\n#define X 'x'\n#else\n#define X '\\0'\n"
                            '#endif\n#define CALL F(X)\n#if CALL\nconst char* d2 = "SR_DEV2";\n#endif\n')
        put('kernel/h.hpp', '#pragma once\n#if SR_ORACLE\ninline const char* h1 = "SR_HDR";\n#endif\n'
                            'inline const char* h2 = "SR_HKEEP";\n')
        put('kernel/o.cpp', 'const char* o = "SR_ORA";\n')
        put('kernel/c.cpp', self.C_CPP)
        put('kernel/lonely.hpp', 'inline const char* l = "SR_LONELY";\n')
        put('tools/difftest/t.cpp', 'const char* t = "SR_ORA";\n')
        put('kernel/lit.cpp', LIT_CPP)
        # Astra round 8, P1-c: sr_imgui is linked into the exe - its reader of a removed gate survives in the release.
        put('kernel/g.cpp', '#if SR_ORACLE\nconst char* g = "SR_IMGUI_READ";\n#endif\n')
        put('ext/im.cpp', 'const char* im = "SR_IMGUI_READ";\n')
        put('ext/fx.cpp', 'const char* fx = "SR_IN_DLL";\n')
        # P2-b: an #embed's bytes (SR_EMBED), a resource script.
        put('kernel/e.cpp', 'const unsigned char blob[] = {\n#embed "blob.bin"\n};\n')
        with open(os.path.join(self.root, 'kernel', 'blob.bin'), 'wb') as fh:
            fh.write(b'SR_EMBED\0')
        put('app/app.rc', '1 ICON "app.ico"\n')
        with open(os.path.join(self.root, 'app', 'app.ico'), 'wb') as fh:
            fh.write(b'\0\0\1\0')
        # P2-a: a unit whose only route to p.hpp is a PCH (-include-pch, no textual -include). Round 9: p.hpp's text
        # depends on its producer's context - SWITCH (Astra's repro) and the <pinc.hpp> an include path resolves.
        put('kernel/p.hpp', '#pragma once\n#include <pinc.hpp>\n#if SR_ORACLE\ninline const char* p1 = "SR_PCH";\n'
                            '#endif\n#ifdef SWITCH\ninline const char* p2 = "SR_PCH_ONLY";\n#endif\n')
        put('pinc/pinc.hpp', '#pragma once\n#define PINC_FROM_A 1\n')
        put('pinc2/pinc.hpp', '#pragma once\n#define PINC_FROM_B 1\n')
        put('kernel/pch_user.cpp', 'int pch_user;\n')
        for b in (self.bd, self.bd_rel):
            with open(os.path.join(b, 'pch', 'p.cxx'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write('/* the PCH source, as CMake writes cmake_pch.hxx.cxx */\n')

    def put(self, rel, text):
        with open(os.path.join(self.root, rel), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)

    def src(self, rel):
        return os.path.join(self.root, rel).replace('\\', '/')

    @staticmethod
    def obj(src, target):
        return (f'guest/CMakeFiles/{target}.dir/{src}.obj' if target == 'SupermanReturnsGuest'
                else f'CMakeFiles/{target}.dir/{src}.obj')

    def entry(self, bd, src, target, v, extra=()):
        obj = self.obj(src, target)
        args = [self.cc, '-std=c++20', f'-DSR_ORACLE={v}'] + list(extra) + ['-c', self.src(src), '-o', obj]
        return {'directory': bd, 'file': self.src(src), 'output': obj, 'arguments': args}

    def db(self, view, drop=(), pch=None, pch_defs=(), user_incs=()):
        """The compile database of a view; `drop` leaves out the (src, target) entries named; pch_defs go to the PCH's
        producer only, user_incs to its consumer only (ahead of the include path both share)."""
        bd, v = (self.bd, 1) if view == 'default' else (self.bd_rel, 0)
        pch = pch or os.path.join(bd, 'pch', 'p.hpp.pch').replace('\\', '/')
        pinc = '-I' + self.src('pinc')
        units = [(s, 'SupermanReturns') for s in self.EXE if not (s == 'kernel/o.cpp' and v == 0)]
        units += [('kernel/b.cpp', 'SupermanReturnsGuest'), ('ext/im.cpp', 'sr_imgui'), ('ext/fx.cpp', 'fx'),
                  ('tools/difftest/t.cpp', 'difftest')]
        out = []
        for s, t in units:
            if (s, t) in drop:
                continue
            extra = []
            if s == 'kernel/pch_user.cpp':   # an extra include path that resolves nothing differently, like 2 units of
                extra = list(user_incs) + ['-I' + self.src('ext'), pinc, '-Xclang', '-include-pch', '-Xclang', pch]
            elif s == 'kernel/a.cpp':       # the tree's (CMake's -Xclang --dependent-lib=msvcrt); depl.lib here
                extra = ['-Xclang', '--dependent-lib=depl']
            out.append(self.entry(bd, s, t, v, extra))
        p_src = os.path.join(bd, 'pch', 'p.cxx').replace('\\', '/')
        out.append({'directory': bd, 'file': p_src, 'output': 'pch/p.hpp.pch',
                    'arguments': [self.cc, '-std=c++20', f'-DSR_ORACLE={v}'] + list(pch_defs)
                    + [pinc, '-Xclang', '-emit-pch', '-Xclang', '-include', '-Xclang', self.src('kernel/p.hpp'), '-x',
                       'c++-header', '-c', p_src, '-o', 'pch/p.hpp.pch']})
        out.append({'directory': bd, 'file': self.src('app/app.rc'),
                    'output': 'CMakeFiles/SupermanReturns.dir/app/app.rc.res',
                    'command': f'llvm-rc -fo CMakeFiles/SupermanReturns.dir/app/app.rc.res {self.src("app/app.rc")}'})
        return out

    def graph(self, view, link_oracle=False, guest=True, exe=True, extra=None, rule=None, exe_rule=None,
              exe_vars=None, order_only=(), other_lib=False, file_vars=None, drop_vars=(), file_lines=(),
              var_obj=False, hidden=False, imgui_vars=None, first_obj=None):
        """build.ninja of a view - a graph ninja itself loads: SupermanReturns.exe's link closure (the release graph
        drops kernel/o.cpp unless link_oracle), with CMake's `include` of its rules (LINK_RULES; `rule` replaces the
        exe's link rule, `exe_rule` names another) and `include extra.ninja` holding `extra`; exe_vars add or replace
        the exe edge's bindings (imgui_vars sr_imgui.lib's), drop_vars leave some out; file_vars / file_lines are
        file-level bindings, in order; var_obj spells one of the exe's objects through a variable ($OBJDIR); hidden
        adds kernel/hidden.cpp's object, built before the link (order-only) and linked only if a binding names it;
        first_obj = (object, source) adds that compile edge and links its object first."""
        rel = view == 'release'
        bd = self.bd if not rel else self.bd_rel
        exe_units = [s for s in self.EXE if not (rel and s == 'kernel/o.cpp' and not link_oracle)]
        rules = dict(LINK_RULES)
        if rule:
            rules['CXX_EXECUTABLE_LINKER__SupermanReturns_Release'] = rule
        with open(os.path.join(bd, 'rules.ninja'), 'w', encoding='utf-8', newline='\n') as fh:
            for t in ('SupermanReturns', 'SupermanReturnsGuest', 'sr_imgui', 'fx'):
                fh.write(f'rule CXX_COMPILER__{t}_unscanned_Release\n  command = clang++ -c $in -o $out\n')
            fh.write('rule RC_COMPILER__SupermanReturns_unscanned_Release\n  command = llvm-rc /fo $out $in\n'
                     'rule CXX_SHARED_LIBRARY_LINKER__fx_Release\n  command = clang++ -shared $in -o $out\n')
            for name, binds in rules.items():
                fh.write(f'rule {name}\n' + ''.join(f'  {k} = {v}\n' for k, v in binds.items()))
        lines = ([f'{k} = {v}' for k, v in (file_vars or {}).items()] + list(file_lines)
                 + (['OBJDIR = CMakeFiles/SupermanReturns.dir'] if var_obj else []) + ['include rules.ninja'])
        if extra:
            with open(os.path.join(bd, 'extra.ninja'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(extra)
            lines.append('include extra.ninja')
        for s in exe_units + (['kernel/hidden.cpp'] if hidden else []):
            lines.append(f'build {self.obj(s, "SupermanReturns")}: CXX_COMPILER__SupermanReturns_unscanned_Release '
                         f'{nj(self.src(s))}')
        if first_obj:
            lines.append(f'build {nj(first_obj[0])}: CXX_COMPILER__SupermanReturns_unscanned_Release '
                         f'{nj(self.src(first_obj[1]))}')
        for s, t in (('kernel/b.cpp', 'SupermanReturnsGuest'), ('ext/im.cpp', 'sr_imgui'), ('ext/fx.cpp', 'fx')):
            lines.append(f'build {self.obj(s, t)}: CXX_COMPILER__{t}_unscanned_Release {nj(self.src(s))}')
        lines += ['build guest/SupermanReturnsGuest.lib: CXX_STATIC_LIBRARY_LINKER__SupermanReturnsGuest_Release '
                  + self.obj('kernel/b.cpp', 'SupermanReturnsGuest'),
                  '  PRE_LINK = cd .', '  POST_BUILD = cd .', '  RSP_FILE = CMakeFiles\\SupermanReturnsGuest.rsp',
                  '  TARGET_FILE = guest\\SupermanReturnsGuest.lib',
                  'build sr_imgui.lib: CXX_STATIC_LIBRARY_LINKER__sr_imgui_Release ' + self.obj('ext/im.cpp', 'sr_imgui')]
        lines += [f'  {k} = {v}' for k, v in dict({'PRE_LINK': 'cd .', 'POST_BUILD': 'cd .',
                                                    'TARGET_FILE': 'sr_imgui.lib'}, **(imgui_vars or {})).items()]
        lines += ['build fx.dll | fx.lib: CXX_SHARED_LIBRARY_LINKER__fx_Release ' + self.obj('ext/fx.cpp', 'fx'),
                  'build CMakeFiles/SupermanReturns.dir/app/app.rc.res: RC_COMPILER__SupermanReturns_unscanned_Release '
                  + nj(self.src('app/app.rc'))]
        if other_lib:    # a static library the graph builds and the exe does not link
            lines.append('build other.lib: CXX_STATIC_LIBRARY_LINKER__sr_imgui_Release ' + self.obj('ext/im.cpp', 'o'))
        libs = (['guest/SupermanReturnsGuest.lib'] if guest else []) + ['sr_imgui.lib', 'fx.lib']
        if exe:
            objs = [self.obj(s, 'SupermanReturns') for s in exe_units]
            if var_obj:
                objs[0] = objs[0].replace('CMakeFiles/SupermanReturns.dir', '$OBJDIR')
            objs = ([nj(first_obj[0])] if first_obj else []) + objs
            later = list(order_only) + ([self.obj('kernel/hidden.cpp', 'SupermanReturns')] if hidden else [])
            lines.append(f'build SupermanReturns.exe: {exe_rule or "CXX_EXECUTABLE_LINKER__SupermanReturns_Release"} '
                         + ' '.join(objs) + ' CMakeFiles/SupermanReturns.dir/app/app.rc.res | ' + ' '.join(libs)
                         + (' || ' + ' '.join(later) if later else ''))
            binds = {'LINK_FLAGS': '-Xlinker /subsystem:console',
                     'LINK_LIBRARIES': '  '.join(libs) + '  -lkernel32.lib', 'PRE_LINK': 'cd .',
                     # CMake's post-build copies next to the exe (the tree's SDL3.dll, data/, licences): harmless
                     'POST_BUILD': f'{CMD}"cd /D {bd} && {CMAKE} -E copy_if_different {self.src("kernel/blob.bin")} '
                                   f'{bd}"',
                     'RSP_FILE': 'CMakeFiles\\SupermanReturns.rsp', 'TARGET_FILE': 'SupermanReturns.exe',
                     'TARGET_IMPLIB': 'SupermanReturns.lib', 'TARGET_PDB': 'SupermanReturns.pdb'}
            binds.update(exe_vars or {})
            lines += [f'  {k} = {v}' for k, v in binds.items() if k not in drop_vars]
        path = os.path.join(bd, 'build.ninja')
        with open(path, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('\n'.join(lines) + '\n')
        return path

    def write_db(self, name, db):
        path = os.path.join(self.root, name)
        with open(path, 'w', encoding='utf-8') as fh:
            json.dump(db, fh)
        return path

    def args(self, d_db=None, r_db=None, **graph):
        """The CLI of a run: both databases and both graphs (the release graph built with `graph`)."""
        d = self.write_db('compdb_default.json', d_db if d_db is not None else self.db('default'))
        r = self.write_db('compdb_release.json', r_db if r_db is not None else self.db('release'))
        return ['--root', self.root, '--compdb', d, '--compdb-rel', r, '--ninja', self.graph('default'),
                '--ninja-rel', self.graph('release', **graph), '--jobs', '2']


ACKS = ('resource app/app.rc', 'dll fx.dll', 'external kernel32.lib', 'embed kernel/e.cpp #embed "blob.bin"',
        'external cfgmgr32.lib', 'linker /DEFAULTLIB:extra.lib', 'external depl.lib')


def run_main(*args):
    out, err = io.StringIO(), io.StringIO()
    old = sys.argv
    sys.argv = ['gate_sweep.py'] + list(args)
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = gs.main()
            except SystemExit as e:      # an argparse error
                rc = e.code
    finally:
        sys.argv = old
    return rc, out.getvalue(), err.getvalue()


def rule_checks(t):
    """Round 9 (Astra round 8, P1): a link rule's own text - command, response file - must not hand the linker an
    input the edge does not list; every case is decided before any unit is preprocessed."""
    EXE_RULE = LINK_RULES['CXX_EXECUTABLE_LINKER__SupermanReturns_Release']
    cases = (
        ('Astra\'s repro: a response file (rspfile_content = $in extra.obj) adds an order-only object',
         dict(rule={'command': 'clang++ @exe.rsp -o $out', 'rspfile': 'exe.rsp', 'rspfile_content': '$in extra.obj'},
              order_only=['extra.obj']), 'extra.obj'),
        ('the rule\'s command names an object of its own', dict(rule={'command': 'clang++ $in extra.obj -o $out'}),
         'extra.obj'),
        ('the rule reads a response file it does not write', dict(rule={'command': 'clang++ @other.rsp $in -o $out'}),
         '@other.rsp'),
        ('the rule hands the linker a /defaultlib', dict(rule={'command': 'clang++ $in -o $out -Xlinker '
                                                                        '/defaultlib:hidden.lib'}), 'hidden.lib'),
        ('an edge binding overrides the rule\'s command (ninja reads the edge\'s first)',
         dict(exe_vars={'command': 'clang++ $in extra2.obj -o $out'}), 'extra2.obj'),
        ('the edge names a rule the graph does not define (ninja itself refuses the graph)',
         dict(exe_rule='CXX_EXECUTABLE_LINKER__Missing'), 'CXX_EXECUTABLE_LINKER__Missing'),
    )
    for label, graph, needle in cases:
        rc, _, err = run_main(*t.args(**graph))
        check('round 9: exit 2 when %s, naming it' % label, rc == 2 and needle in err and 'release build' in err, err)
    # Round 10 (Astra round 9, P1): the tokens are the linker's (response-file quoting decoded), and a link binding the
    # edge does not hold itself - the rule's or the file's, which the closure never reads - is not trusted.
    cases = (
        ('a response file names a quoted "extra.obj" (Astra\'s repro)',
         dict(rule={'command': 'clang++ @exe.rsp -o $out', 'rspfile': 'exe.rsp', 'rspfile_content': '$in "extra.obj"'},
              order_only=['extra.obj']), 'extra.obj'),
        ('the rule\'s command names a quoted "extra.obj"', dict(rule={'command': 'clang++ $in "extra.obj" -o $out'}),
         'extra.obj'),
        ('a file-level LINK_LIBRARIES the response file reads (Astra\'s repro: the edge binds none)',
         dict(file_vars={'LINK_LIBRARIES': 'extra.obj'}, drop_vars=('LINK_LIBRARIES',)), 'extra.obj'),
        ('a rule-level LINK_LIBRARIES', dict(rule=dict(EXE_RULE, LINK_LIBRARIES='extra.obj'),
                                             drop_vars=('LINK_LIBRARIES',)), 'extra.obj'),
        ('a flag this tool does not know (one that makes the driver link a profile runtime)',
         dict(rule={'command': 'clang++ $in -o $out -fprofile-instr-generate'}), '-fprofile-instr-generate'),
    )
    for label, graph, needle in cases:
        rc, _, err = run_main(*t.args(**graph))
        check('round 10: exit 2 when %s, naming it' % label, rc == 2 and needle in err and 'release build' in err, err)
    # Round 11 (Astra round 10): the link command is ninja's own expansion (`ninja -t compdb -x`), so ninja's binding
    # semantics decide - a file-level assignment is frozen when ninja reads it (Astra's repro: EXTRA = extra.obj,
    # LINK_LIBRARIES = $EXTRA, EXTRA = still links extra.obj) - and each linker edge must read in ninja (`-t query`) as
    # the closure read it (a variable in a build line's path).
    cases = (
        ('a file-level LINK_LIBRARIES = $EXTRA whose EXTRA is emptied afterwards (Astra\'s repro: ninja keeps extra.obj)',
         dict(file_lines=['EXTRA = extra.obj', 'LINK_LIBRARIES = $EXTRA', 'EXTRA ='], drop_vars=('LINK_LIBRARIES',)),
         'extra.obj'),
        ('a linker edge\'s input is spelled through a variable the closure\'s reader does not expand',
         dict(var_obj=True), 'differently from the closure'),
    )
    for label, graph, needle in cases:
        rc, _, err = run_main(*t.args(**graph))
        check('round 11: exit 2 when %s, naming it' % label, rc == 2 and needle in err and 'release build' in err, err)
    # Round 11 (Astra round 10, P2): $in_newline in a response file is a supported arrangement - not rejected.
    rc, _, err = run_main(*t.args(rule=dict(EXE_RULE, rspfile_content='$in_newline $LINK_PATH $LINK_LIBRARIES')))
    check('round 11: a response file of $in_newline (one input per line) passes the link check',
          'release build' not in err and 'its link command' not in err, err)
    # Round 12 (Astra round 11): a linker edge must be one of the tree's link command templates, filled with ninja's
    # own values of its variables - never ninja's -x expansion (an unbounded substring swap of @rspfile), never a
    # shell's reading of a response file, no step between or after two links. Astra's three repros (each passed
    # a2db69e4's checks) and their CMake-template forms, an edge's own rspfile / rspfile_content, and steps.
    libs = 'guest/SupermanReturnsGuest.lib  sr_imgui.lib  fx.lib  -lkernel32.lib'
    a_obj = t.obj('kernel/a.cpp', 'SupermanReturns')
    blob = t.src('kernel/blob.bin')
    cases = (
        ('the linker reads @exe.rsp.lib and ninja writes exe.rsp (Astra\'s @x.rsp.more repro: `compdb -x` swapped '
         'exe.rsp\'s content in - its last token -lkernel32 then read -lkernel32.lib, a link flag of the edge\'s)',
         dict(rule={'command': 'clang++ -o $out @exe.rsp.lib', 'rspfile': 'exe.rsp',
                    'rspfile_content': '$in $LINK_LIBRARIES'},
              exe_vars={'LINK_FLAGS': '-lkernel32.lib', 'LINK_LIBRARIES': libs[:-len('.lib')]}), 'exe.rsp.lib'),
        ('a link flag of the CMake template names @<its response file>.more',
         dict(exe_vars={'LINK_FLAGS': '-Xlinker /subsystem:console @CMakeFiles\\SupermanReturns.rsp.more'}),
         'SupermanReturns.rsp.more'),
        ('a response file holds `&&cd hidden.obj` (Astra\'s repro: the linker reads `<last input>&&cd` and '
         'hidden.obj, a shell reading saw `cd hidden.obj`)',
         dict(rule={'command': 'clang++ -o $out @exe.rsp', 'rspfile': 'exe.rsp',
                    'rspfile_content': '$in&&cd hidden.obj'}), 'hidden.obj'),
        ('the CMake template\'s response file holds `&&` (the edge\'s LINK_PATH)',
         dict(exe_vars={'LINK_PATH': '-Lx&&cd'}), "holds '&'"),
        ('a copy between two links (Astra\'s repro: hidden.obj copied over an object the second link reads)',
         dict(rule={'command': f'clang++ $in -o $out && {CMAKE} -E copy hidden.obj {a_obj} && clang++ $in -o $out'}),
         'hidden.obj'),
        ('a static library\'s post-build copy over an object the exe links afterwards',
         dict(imgui_vars={'POST_BUILD': f'{CMD}"{CMAKE} -E copy {blob} {t.bd_rel}/{a_obj}"'}),
         'sr_imgui.lib: a post-build step'),
        ('the exe\'s post-build step links again', dict(exe_vars={
            'POST_BUILD': f'{CMD}"cd /D {t.bd_rel} && {LD} -o {t.bd_rel}/SupermanReturns.exe {t.bd_rel}/{a_obj}"'}),
         'a post-build step this tool does not accept'),
        ('an edge binding of rspfile (ninja writes another file than the linker reads)',
         dict(exe_vars={'rspfile': 'CMakeFiles\\other.rsp'}), 'other.rsp'),
        ('an edge binding of rspfile_content (ninja reads the edge\'s before the rule\'s)',
         dict(exe_vars={'rspfile_content': 'extra.obj'}), 'extra.obj'),
        ('a post-build command that is not a copy', dict(exe_vars={'POST_BUILD': 'tool.exe SupermanReturns.exe'}),
         'a post-build step (tool.exe SupermanReturns.exe)'),
        ('a post-build copy over the exe (CMake\'s form)', dict(exe_vars={
            'POST_BUILD': f'{CMD}"cd /D {t.bd_rel} && {CMAKE} -E copy_if_different {t.src("ext/SupermanReturns.exe")} '
                          f'{t.bd_rel}"'}), 'a post-build step this tool does not accept'),
        ('a file-level PRE_LINK (the edge binds none: the one the command runs)',
         dict(file_vars={'PRE_LINK': 'tool.exe patch-objects'}, drop_vars=('PRE_LINK',)),
         'a pre-link step (tool.exe patch-objects)'),
    )
    for label, graph, needle in cases:
        rc, _, err = run_main(*t.args(**graph))
        check('round 12: exit 2 when %s, naming it' % label, rc == 2 and needle in err and 'release build' in err, err)
    for rsp in ('"@$RSP_FILE"', '@"$RSP_FILE"'):
        rc, _, err = run_main(*t.args(rule=dict(EXE_RULE, command=EXE_RULE['command'].replace('@$RSP_FILE', rsp))))
        check(f'round 12: a quoted response-file argument ({rsp}) naming the edge\'s rspfile exactly passes the link '
              f'check (Astra\'s P2)', 'release build' not in err and 'its link command' not in err, err)
    # Round 13 (Astra round 12, P1): a step or link argument naming a file through a Windows path alias is exit 2, and
    # a post-build copy's destination is compared by resolved identity (a directory junction, a hard link).
    exe_win = os.path.join(t.bd_rel, 'SupermanReturns.exe')
    drive, rest = os.path.splitdrive(exe_win)

    def copy_to(dest):
        return dict(exe_vars={'POST_BUILD': f'{CMD}"cd /D {t.bd_rel} && {CMAKE} -E copy {blob} {dest}"'})

    for label, dest in (('a \\\\?\\ namespace prefix (Astra\'s repro)', '\\\\?\\' + exe_win),
                        ('a \\\\.\\ device prefix', '\\\\.\\' + exe_win), ('an NT \\??\\ prefix', '\\??\\' + exe_win),
                        ('a UNC administrative share', f'\\\\localhost\\{drive[:1]}$${rest}'),
                        ('an 8.3 short name', os.path.join(t.bd_rel, 'SUPERM~1.EXE')),
                        ('a trailing dot', exe_win + '.'), ('an alternate data stream', exe_win + '::$$DATA')):
        rc, _, err = run_main(*t.args(**copy_to(dest)))
        check(f'round 13: exit 2 when a post-build copy names the exe through {label}',
              rc == 2 and 'a path this tool does not resolve' in err and 'release build' in err, err)
    rc, _, err = run_main(*t.args(exe_vars={'LINK_FLAGS': '-Xlinker /subsystem:console -L\\\\?\\' + t.bd_rel}))
    check('round 13: exit 2 when a link argument names a directory through a namespace prefix',
          rc == 2 and 'a path this tool does not resolve' in err and 'release build' in err, err)
    junction = os.path.join(t.root, 'junction')
    try:
        import _winapi
        _winapi.CreateJunction(t.bd_rel, junction)
    except (ImportError, OSError) as e:
        check('round 13: a directory junction can be made for the identity check', False, e)
    else:
        try:
            rc, _, err = run_main(*t.args(**copy_to(os.path.join(junction, 'SupermanReturns.exe'))))
        finally:
            os.rmdir(junction)                                   # the junction only
        check('round 13: exit 2 when a post-build copy names the exe through a directory junction (its resolved '
              'identity)', rc == 2 and 'a post-build step this tool does not accept' in err, err)
    linked = os.path.join(t.root, 'hardlink.exe')
    with open(exe_win, 'wb') as fh:
        fh.write(b'MZ')
    try:
        os.link(exe_win, linked)
        rc, _, err = run_main(*t.args(**copy_to(linked)))
    finally:
        for f in (linked, exe_win):
            if os.path.exists(f):
                os.remove(f)
    check('round 13: exit 2 when a post-build copy writes the exe through a hard link of it (the same file)',
          rc == 2 and 'a post-build step this tool does not accept' in err, err)
    # Round 13 (Astra round 12, P1): a BOM before the first input's name (clang drops it from the response file and
    # links the object without one; the closure read the name with it), or any non-ASCII name, is exit 2.
    hid = t.obj('kernel/hidden.cpp', 'SupermanReturns')
    for label, name in (('a UTF-8 BOM begins the response file (Astra\'s repro: the first input\'s name)', '﻿' + hid),
                        ('an input name holds a non-ASCII character', hid.replace('hidden', 'hiddén'))):
        rc, _, err = run_main(*t.args(first_obj=(name, 'kernel/c.cpp'), hidden=True))
        check(f'round 13: exit 2 when {label}', rc == 2 and 'non-ASCII' in err and 'release build' in err, err)
    env = {k: v for k, v in os.environ.items() if k not in ('PYTHONIOENCODING', 'PYTHONUTF8')}
    p = subprocess.run([sys.executable, os.path.join(HERE, 'gate_sweep.py')]
                       + t.args(first_obj=('﻿' + hid, 'kernel/c.cpp'), hidden=True), capture_output=True, env=env)
    check('round 13: the command line exits 2 - no traceback - when a problem quotes a non-ASCII name (stderr in the '
          'console\'s code page)', p.returncode == 2 and b'non-ASCII' in p.stderr, p.stderr[-300:])


def structure_checks(base, cc):
    """The release is the exe's link closure in each build's graph, and every linked unit must be in its build's
    compile database (Astra round 8, P1-b / P1-c / P2-a): each case is decided before any unit is preprocessed, so
    these run without a compiler too."""
    t = Tree(base, cc or 'clang++')
    rc, _, err = run_main(*t.args(r_db=[]))
    check('P1-b: an empty release compile database is exit 2 (every linked unit lacks its entry)',
          rc == 2 and 'the compile database has no entry' in err and 'release build' in err, err)
    rc, _, err = run_main(*t.args(r_db=t.db('release', drop={('kernel/b.cpp', 'SupermanReturnsGuest')})))
    check('P1-b: a release database without SupermanReturnsGuest\'s unit is exit 2, naming it',
          rc == 2 and 'guest/CMakeFiles/SupermanReturnsGuest.dir/kernel/b.cpp.obj' in err, err)
    rc, _, err = run_main(*t.args(guest=False))
    check('P1-b: a release graph whose exe does not link SupermanReturnsGuest is exit 2 (a required target)',
          rc == 2 and 'required target SupermanReturnsGuest' in err, err)
    rc, _, err = run_main(*t.args(r_db=t.db('release', drop={('ext/im.cpp', 'sr_imgui')})))
    check('P1-c: sr_imgui is in the closure - a release database without its unit is exit 2, naming it',
          rc == 2 and 'ext/im.cpp' in err and 'no entry' in err, err)
    rc, _, err = run_main(*t.args(link_oracle=True))
    check('P1-c: the graph tells an ORACLE_SOURCES unit apart - linked by the release graph with no release entry is '
          'exit 2, not an empty view', rc == 2 and 'kernel/o.cpp' in err and 'no entry' in err, err)
    wrong = t.db('release')
    for e in wrong:
        if e['file'].endswith('kernel/c.cpp'):
            e['file'] = t.src('kernel/a.cpp')
    rc, _, err = run_main(*t.args(r_db=wrong))
    check('P1-c: an entry at a linked object\'s path for another source is exit 2',
          rc == 2 and 'the build graph compiles kernel/c.cpp' in err, err)
    rc, _, err = run_main(*t.args(exe=False))
    check('P1-c: a release graph with no SupermanReturns.exe edge is exit 2', rc == 2 and 'SupermanReturns.exe' in err,
          err)
    rc, _, err = run_main(*t.args(extra='build CMakeFiles/SupermanReturns.dir/x.obj: CXX_COMPILER__x x.cpp\n'))
    check('P1-c: an included ninja file holding build edges (the reader reads one file) is exit 2',
          rc == 2 and 'include extra.ninja holds build edges' in err, err)
    prebuilt = os.path.join(t.bd_rel, 'prebuilt.pch').replace('\\', '/')
    rc, _, err = run_main(*t.args(r_db=t.db('release', pch=prebuilt)))
    check('P2-a: a PCH no database entry produces (a prebuilt .pch) is exit 2, naming the unit',
          rc == 2 and 'kernel/pch_user.cpp' in err and 'prebuilt.pch' in err, err)
    rule_checks(t)
    a = t.args()
    rc, _, err = run_main(*[x for k, x in enumerate(a) if k < 6 or k > 9])
    check('the clang views need both build graphs (exit 2 without --ninja / --ninja-rel)',
          rc == 2 and '--ninja' in err, err)
    pr = gs.load_receipts() if hasattr(gs, 'resolve_pch') else None
    if pr:
        db = {gs.canon(os.path.join(t.bd, 'x.hxx.pch')): {
            'directory': t.bd, 'file': os.path.join(t.bd, 'x.hxx.cxx'), 'output': 'x.hxx.pch',
            'arguments': ['clang++', '-Xclang', '-emit-pch', '-include', 'x.hxx', '-c', 'x.hxx.cxx', '-o',
                          'x.hxx.pch']}}
        got = gs.resolve_pch(['clang++', '-Xclang', '-include-pch', '-Xclang', 'x.hxx.pch', '-Xclang', '-include',
                              '-Xclang', 'x.hxx', 'u.cpp'], t.bd, db, pr)
        got = got[0] if isinstance(got, tuple) else got
        want = ['clang++', '-Xclang', '-include', '-Xclang', os.path.join(t.bd, 'x.hxx'), '-Xclang', '-include',
                '-Xclang', os.path.join(t.bd, 'x.hxx.cxx'), '-Xclang', '-include', '-Xclang', 'x.hxx', 'u.cpp']
        check('P2-a: CMake\'s PCH form reads as the PCH\'s text (its forced include, then its source), then the '
              'unit\'s own includes', got == want, got)
        got = gs.textual_includes(['clang++', '-include', 'a.h', '--include=b.h', '-includec.h', '-include-pch', 'p'])
        check('P2-a: a driver -include (which would swap in a .pch next to the header) becomes cc1\'s textual include',
              got == ['clang++'] + ['-Xclang', '-include', '-Xclang', 'a.h', '-Xclang', '-include', '-Xclang', 'b.h',
                                    '-Xclang', '-include', '-Xclang', 'c.h', '-include-pch', 'p'], got)
        chained = dict(db)
        k = next(iter(chained))
        chained[k] = dict(chained[k], arguments=chained[k]['arguments'] + ['-include-pch', 'y.pch'])
        try:
            r = gs.resolve_pch(['clang++', '-include-pch', 'x.hxx.pch', 'u.cpp'], t.bd, chained, pr)
        except gs.Unmodelled as e:
            r = 'exit 2: %s' % e
        check('P2-a: a PCH built from another PCH is exit 2', isinstance(r, str), r)
        try:
            r = gs.resolve_pch(['clang++', '-Xclang', '-include', '-Xclang', 'a.h', '-include-pch', 'x.hxx.pch',
                                'u.cpp'], t.bd, db, pr)
        except gs.Unmodelled as e:
            r = 'exit 2: %s' % e
        check('round 9: a forced include ahead of the PCH (clang reads the PCH first) is exit 2', isinstance(r, str), r)
    else:
        check('P2-a: the PCH is read as text', False, 'no resolve_pch')
    acks = os.path.join(base, 'acks.txt')
    with open(acks, 'w', encoding='utf-8') as fh:
        fh.write('# a comment\n\nresource app/app.rc\n  dll fx.dll  \n')
    check('--uncovered-ok: one item per line, `#` lines and blanks skipped',
          hasattr(gs, 'read_acks') and gs.read_acks(acks) == {'resource app/app.rc', 'dll fx.dll'})


def clang_mode_checks(cc, base):
    """The authority (clang views), on the scratch tree: Astra round 7's two repros, which the lexer cannot see
    through, are decided by clang; a header through its including unit; an ORACLE_SOURCES unit the release graph drops,
    its name surviving in a release unit and a difftest source; round 8's literal values (each confirmed in the
    object clang compiles), sr_imgui's survivor, the PCH's text, and the UNCOVERED inputs (a resource script, a DLL,
    a system library, an #embed) with their acknowledgement; round 9's non-ASCII literal suffixes, #pragma comment
    and --dependent-lib requests, PCH contexts and post-build steps; a header no unit includes (unreachable); a unit
    that fails to preprocess or holds an undecodable literal, and a missing compiler (exit 2)."""
    t = Tree(base, cc)
    obj = os.path.join(base, 'lit.obj')
    p = subprocess.run([cc, '-std=c++20', '-DSR_ORACLE=1', '-c', t.src('kernel/lit.cpp'), '-o', obj],
                       capture_output=True, text=True)
    data = open(obj, 'rb').read() if p.returncode == 0 else b''
    check('clang confirms P1-a\'s values: the object it compiles holds each name as a C string',
          all((n + '\0').encode() in data for n in LIT_NAMES), p.stderr[-300:])
    acks = os.path.join(base, 'uncovered_ok.txt')
    with open(acks, 'w', encoding='utf-8') as fh:
        fh.write('# the scratch tree\'s uncovered inputs, judged\n' + '\n'.join(ACKS) + '\n')
    js = os.path.join(base, 'clang.json')
    rc, out, err = run_main(*t.args(), '--json', js, '--uncovered-ok', acks)
    res = json.load(open(js, encoding='utf-8')) if rc == 0 else {}
    check('clang views run (exit 0, every UNCOVERED input acknowledged)', rc == 0, err or out)
    removed = res.get('removed', {})
    check('Astra repro 1 (`#define CALL F(` + `#if CALL X)`): clang removes SR_DEV from kernel/a.cpp',
          removed.get('SR_DEV') == ['kernel/a.cpp'], removed)
    check('Astra repro 2 (`#define CALL F(X)` + `#if CALL`, X a character literal per SR_ORACLE): clang removes SR_DEV2',
          removed.get('SR_DEV2') == ['kernel/b.cpp'], removed)
    check('a header is read through the unit that includes it: SR_HDR removed from kernel/h.hpp, SR_HKEEP kept',
          removed.get('SR_HDR') == ['kernel/h.hpp'] and 'SR_HKEEP' not in removed, removed)
    check('an ORACLE_SOURCES unit (the release graph does not link it) has its names removed',
          removed.get('SR_ORA') == ['kernel/o.cpp'], removed)
    check('P1-a: each literal value is removed from kernel/lit.cpp',
          all(removed.get(n) == ['kernel/lit.cpp'] for n in LIT_NAMES), removed)
    check('P2-a: the PCH\'s text is read - SR_PCH removed from kernel/p.hpp, which only the PCH brings in',
          removed.get('SR_PCH') == ['kernel/p.hpp'], removed)
    surviving = res.get('surviving', {})
    check('SR_ORA survives in a release unit [REL] and a difftest source [NOT-REL]',
          surviving.get('SR_ORA') == [['kernel/c.cpp', 'REL'], ['tools/difftest/t.cpp', 'NOT-REL']], surviving)
    check('P1-c: SR_IMGUI_READ survives in sr_imgui\'s unit [REL] (the exe links it)',
          surviving.get('SR_IMGUI_READ') == [['ext/im.cpp', 'REL']], surviving)
    check('no other name is removed or survives (SR_KEPT, SR_HKEEP; SR_EMBED is in bytes, SR_IN_DLL in the DLL)',
          set(removed) == {'SR_DEV', 'SR_DEV2', 'SR_HDR', 'SR_ORA', 'SR_PCH', 'SR_IMGUI_READ', *LIT_NAMES}
          and set(surviving) == {'SR_ORA', 'SR_IMGUI_READ'}, sorted(removed))
    st = res.get('stats', {})
    check('the closure: SupermanReturns.exe, SupermanReturnsGuest.lib and sr_imgui.lib units in both views; the '
          'release graph drops kernel/o.cpp',
          st.get('components', {}).get('release') == {'SupermanReturns.exe': 7, 'SupermanReturnsGuest.lib': 1,
                                                       'sr_imgui.lib': 1}
          and st.get('dropped_by_release') == ['kernel/o.cpp'] and st.get('units_not_rel') == 1, st)
    check('round 9: the PCH\'s text reads the same under its producer\'s command and its consumer\'s (one context per '
          'view; the consumer\'s extra include path resolves nothing differently)',
          st.get('pch_contexts_checked') == 2 and 'SR_PCH_ONLY' not in removed, st)
    unc = {u['item']: u for u in res.get('uncovered', [])}
    check('P2-b: the UNCOVERED inputs are listed in the JSON, each with both views and acknowledged',
          set(unc) == set(ACKS) and all(u['views'] == ['default', 'release'] and u['acknowledged']
                                        for u in unc.values()), res.get('uncovered'))
    check('round 9: #pragma comment(lib) and comment(linker) (via _Pragma) are UNCOVERED inputs naming their file; one '
          'for a library the graph builds and the exe links (sr_imgui.lib) is not; --dependent-lib asks for one too',
          'kernel/c.cpp' in unc.get('external cfgmgr32.lib', {}).get('detail', '')
          and 'kernel/c.cpp' in unc.get('linker /DEFAULTLIB:extra.lib', {}).get('detail', '')
          and '--dependent-lib' in unc.get('external depl.lib', {}).get('detail', '')
          and not any('sr_imgui' in k for k in unc), res.get('uncovered'))
    check('round 9: CMake\'s post-build copy that writes no file a linker edge reads or writes passes the link check '
          '(no UNCOVERED item, exit 0)', rc == 0 and not any(k.startswith('step ') for k in unc), list(unc))
    check('P2-b: the report names each UNCOVERED input and exe_sweep on the built binary as their authority',
          'exe_sweep.py on the built binary' in out and all(a in out for a in ACKS)
          and 'exe_sweep.py on the built binary' in res.get('uncovered_authority', ''), out)
    check('P2-b: the resource script\'s detail names its payload',
          'payload app.ico' in unc.get('resource app/app.rc', {}).get('detail', ''), unc.get('resource app/app.rc'))
    check('a header no unit includes is reported unreachable with its names',
          res.get('unreachable', {}).get('kernel/lonely.hpp') == ['SR_LONELY'], res.get('unreachable'))
    adv = res.get('advisory_lexer', {})
    check('the advisory lexer is compared, never used: it cannot see the repros (it differs, or fails closed) and '
          'the exit code is still the one clang decided',
          rc == 0 and ('error' in adv or {'SR_DEV', 'SR_DEV2'} <= set(adv.get('removed_differs', []))), adv)
    rc, out, err = run_main(*t.args())
    check('P2-b: without --uncovered-ok every UNCOVERED input is exit 2, named (the report is still printed)',
          rc == 2 and all(a in err for a in ACKS) and 'NOT ACKNOWLEDGED' in out, err)
    with open(acks, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(a for a in ACKS if not a.startswith('embed')) + '\ndll gone.dll\n')
    rc, out, err = run_main(*t.args(), '--uncovered-ok', acks)
    check('P2-b: an #embed not acknowledged is exit 2, naming it; a stale acknowledgement is reported',
          rc == 2 and 'embed kernel/e.cpp' in err and 'resource app/app.rc' not in err
          and 'no longer uncovered: dll gone.dll' in out, err + out)
    with open(acks, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(ACKS) + '\n')
    # Round 9 (Astra round 8, P2): a PCH built in another preprocessing context - its producer's -DSWITCH=1 (Astra's
    # repro: the PCH holds SR_PCH_ONLY, and carries SWITCH into the unit), or an include path of the consumer's that
    # resolves <pinc.hpp> elsewhere - cannot be read as text in the unit's context: exit 2.
    for label, kw in (('its producer\'s -DSWITCH=1 (Astra\'s repro)', {'pch_defs': ['-DSWITCH=1']}),
                      ('an include path of the unit\'s that resolves the PCH\'s <pinc.hpp> elsewhere',
                       {'user_incs': ['-I' + t.src('pinc2')]})):
        rc, _, err = run_main(*t.args(d_db=t.db('default', **kw), r_db=t.db('release', **kw)), '--uncovered-ok', acks)
        check('round 9: exit 2 when a unit\'s PCH was built in another context - %s - naming the unit' % label,
              rc == 2 and 'kernel/pch_user.cpp' in err and 'another preprocessing context' in err, err)
    # Round 9 (Astra round 8, P1): #pragma comment(lib) naming a library the graph builds and the exe does not link.
    t.put('kernel/c.cpp', t.C_CPP + '#pragma comment(lib, "other.lib")\n')
    rc, _, err = run_main(*t.args(other_lib=True), '--uncovered-ok', acks)
    check('round 9: exit 2 when a #pragma comment(lib) asks for a library the graph builds and the exe does not link',
          rc == 2 and 'other.lib' in err and 'kernel/c.cpp' in err, err)
    t.put('kernel/c.cpp', t.C_CPP)
    # Round 10 (Astra round 9, P2): text that depends on its place - __INCLUDE_LEVEL__ is 0 in the PCH's own source
    # and 1 once the substitution force-includes it - is a PCH that cannot be read as text.
    for b in (t.bd, t.bd_rel):
        with open(os.path.join(b, 'pch', 'p.cxx'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('#define STR_I(x) #x\n#define STR(x) STR_I(x)\n'
                     'inline const char* lvl = "SR_PCH_" STR(__INCLUDE_LEVEL__);\n')
    rc, _, err = run_main(*t.args(), '--uncovered-ok', acks)
    check('round 10: exit 2 when the PCH\'s text depends on its place (__INCLUDE_LEVEL__ 0 in the producer, 1 as the '
          'substitution includes it), naming the unit',
          rc == 2 and 'kernel/pch_user.cpp' in err and 'another preprocessing context' in err, err)
    for b in (t.bd, t.bd_rel):
        with open(os.path.join(b, 'pch', 'p.cxx'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('/* the PCH source, as CMake writes cmake_pch.hxx.cxx */\n')
    # Round 11 (Astra round 10, P2): the PCH comparison reads clang's -E -P output as tokens, literals whole - a line
    # of a raw string that looks like a line marker (`# 1 SR_PCH_HIDDEN`) is the literal's content, not a marker.
    for b in (t.bd, t.bd_rel):
        with open(os.path.join(b, 'pch', 'p.cxx'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('#if __INCLUDE_LEVEL__ == 0\ninline const char* lvl = R"(\n# 1 SR_PCH_HIDDEN\n)";\n#else\n'
                     'inline const char* lvl = R"(\n# 1 SR_PCH_OTHER\n)";\n#endif\n')
    rc, _, err = run_main(*t.args(), '--uncovered-ok', acks)
    check('round 11: exit 2 when the PCH\'s text differs only inside a raw string whose lines look like line markers '
          '(Astra\'s repro), naming the unit',
          rc == 2 and 'kernel/pch_user.cpp' in err and 'another preprocessing context' in err, err)
    for b in (t.bd, t.bd_rel):
        with open(os.path.join(b, 'pch', 'p.cxx'), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write('/* the PCH source, as CMake writes cmake_pch.hxx.cxx */\n')
    # Round 11 (Astra round 10, P1), end to end through exe_sweep: kernel/hidden.cpp names SR_HIDDEN in its default
    # view only, and an ordinary release unit keeps the name; both graphs link hidden.cpp's object through a file-level
    # LINK_LIBRARIES = $EXTRA whose EXTRA is emptied afterwards (ninja keeps the object). A sweep that misses the object
    # never lists SR_HIDDEN as removed, so exe_sweep never looks for it in a binary that holds SR_HIDDEN\0; the
    # pipeline must stop - gate_sweep exit 2 - or exe_sweep must find the name.
    t.put('kernel/hidden.cpp', '#if SR_ORACLE\nconst char* hid = "SR_HIDDEN";\n#endif\n')
    t.put('kernel/c.cpp', t.C_CPP + 'const char* keep = "SR_HIDDEN";  // an ordinary release reader keeps it\n')
    hidden_obj = t.obj('kernel/hidden.cpp', 'SupermanReturns')

    def end_to_end(label, g_default, g_release, needle, d_extra=(), r_extra=()):
        """gate_sweep on both graphs, then - if it passes - exe_sweep on its removed list and a release binary that
        holds SR_HIDDEN\\0: the pipeline must stop (gate_sweep exit 2, naming needle) or exe_sweep must find it."""
        d = t.write_db('compdb_default.json', t.db('default') + [t.entry(t.bd, 'kernel/hidden.cpp', 'SupermanReturns',
                                                                         1)] + list(d_extra))
        r = t.write_db('compdb_release.json', t.db('release') + [
            t.entry(t.bd_rel, 'kernel/hidden.cpp', 'SupermanReturns', 0)] + list(r_extra))
        js = os.path.join(base, 'hidden.json')
        if os.path.exists(js):
            os.remove(js)
        rc, out, err = run_main('--root', t.root, '--compdb', d, '--compdb-rel', r, '--ninja',
                                t.graph('default', **g_default), '--ninja-rel', t.graph('release', **g_release),
                                '--jobs', '2', '--json', js, '--uncovered-ok', acks)
        exe_rc = None
        if rc == 0:
            fake = os.path.join(base, 'release_with_hidden.exe')
            with open(fake, 'wb') as fh:
                fh.write(b'MZ\0\0' + b'\0' * 60 + b'SR_HIDDEN\0')    # the release binary: SR_HIDDEN's reader kept
            spec = importlib.util.spec_from_file_location('exe_sweep_t', os.path.join(HERE, 'exe_sweep.py'))
            es = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(es)
            old, sys.argv = sys.argv, ['exe_sweep.py', '--removed', js, '--release', fake]
            try:
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    exe_rc = es.main()
            finally:
                sys.argv = old
        check(label, (rc == 2 and needle in err) or exe_rc == 1,
              f'gate_sweep exit {rc}, exe_sweep exit {exe_rc}: {ascii(err[-400:])}')

    g = dict(hidden=True, file_lines=['EXTRA = ' + hidden_obj, 'LINK_LIBRARIES = $EXTRA', 'EXTRA ='],
             drop_vars=('LINK_LIBRARIES',))
    end_to_end('round 11: Astra\'s EXTRA repro end to end - gate_sweep exits 2 naming hidden.cpp\'s object, rather '
               'than hand exe_sweep a removed list without SR_HIDDEN (which then passes a binary holding SR_HIDDEN\\0)',
               g, g, 'hidden.cpp.obj')
    # Round 13 (Astra round 12), end to end: the release exe's post-build step copies a payload (hidden.cpp's reader
    # linked in) over the exe through \\?\ - the binary exe_sweep reads is not the one the closure describes.
    payload = (f'{CMD}"cd /D {t.bd_rel} && {CMAKE} -E copy {t.src("ext/payload.exe")} '
               f'\\\\?\\{t.bd_rel}\\SupermanReturns.exe"')
    end_to_end('round 13: Astra\'s path-alias repro end to end - a post-build copy of a payload over the exe through '
               '\\\\?\\ is exit 2, not a removed list without SR_HIDDEN for a binary holding it',
               dict(hidden=True), dict(hidden=True, exe_vars={'POST_BUILD': payload}),
               'a path this tool does not resolve')
    # ... and a UTF-8 BOM before the exe's first input: clang drops it from the response file and links hidden.cpp's
    # object, while the closure reads the BOM-named object - kernel/bomalias.cpp's.
    t.put('kernel/bomalias.cpp', 'int bomalias;\n')
    bom = dict(hidden=True, first_obj=('\ufeff' + hidden_obj, 'kernel/bomalias.cpp'))

    def bom_entry(bd, v):
        e = t.entry(bd, 'kernel/bomalias.cpp', 'SupermanReturns', v)
        return dict(e, output='\ufeff' + hidden_obj, arguments=e['arguments'][:-1] + ['\ufeff' + hidden_obj])

    end_to_end('round 13: Astra\'s response-file BOM repro end to end - a BOM before the first input is exit 2, not '
               'a removed list without SR_HIDDEN for a binary that links hidden.cpp', bom, bom, 'non-ASCII',
               [bom_entry(t.bd, 1)], [bom_entry(t.bd_rel, 0)])
    os.remove(os.path.join(t.root, 'kernel', 'bomalias.cpp'))
    os.remove(os.path.join(t.root, 'kernel', 'hidden.cpp'))
    t.put('kernel/c.cpp', t.C_CPP)
    d_db, r_db = t.db('default'), t.db('release')
    t.EXE = t.EXE + ['kernel/bad.cpp']
    t.put('kernel/bad.cpp', '#if !SR_ORACLE\n#error "the release view of this unit does not preprocess"\n#endif\n')
    rc, _, err = run_main(*t.args(d_db=d_db + [t.entry(t.bd, 'kernel/bad.cpp', 'SupermanReturns', 1)],
                                  r_db=r_db + [t.entry(t.bd_rel, 'kernel/bad.cpp', 'SupermanReturns', 0)]),
                          '--uncovered-ok', acks)
    check('exit 2 when a unit fails to preprocess, naming it', rc == 2 and 'kernel/bad.cpp' in err, err)
    t.put('kernel/bad.cpp', 'const char* bad = "SR_\\x{zz}";\n')
    rc, _, err = run_main(*t.args(d_db=d_db + [t.entry(t.bd, 'kernel/bad.cpp', 'SupermanReturns', 1)],
                                  r_db=r_db + [t.entry(t.bd_rel, 'kernel/bad.cpp', 'SupermanReturns', 0)]),
                          '--uncovered-ok', acks)
    check('P1-a: exit 2 when a unit\'s literal cannot be decoded (\\x{zz}), naming the unit',
          rc == 2 and 'kernel/bad.cpp' in err and 'x{zz}' in err, err)
    t.EXE = t.EXE[:-1]
    os.remove(os.path.join(t.root, 'kernel/bad.cpp'))
    nocc = [dict(e, arguments=[os.path.join(base, 'no_such_dir', 'clang++.exe')] + e['arguments'][1:])
            if 'arguments' in e else e for e in r_db]
    rc, _, err = run_main(*t.args(r_db=nocc), '--uncovered-ok', acks)
    check('exit 2 when the compiler cannot be run', rc == 2 and 'cannot be run' in err, err)
    rc, _, err = run_main('--root', t.root)
    check('exit 2 without the two builds (the lexer alone is --advisory)', rc == 2 and '--advisory' in err, err)


def tree_checks(base):
    root = os.path.join(base, 'tree')
    os.makedirs(os.path.join(root, 'kernel'))
    os.makedirs(os.path.join(root, 'tools', 'difftest'))

    def put(rel, text):
        with open(os.path.join(root, rel), 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)

    put('CMakeLists.txt', 'set(APP_SOURCES\n  kernel/a.cpp\n  kernel/o.cpp\n)\nset(ORACLE_SOURCES\n  kernel/o.cpp\n)\n')
    put('kernel/a.cpp', '#if SR_ORACLE\nconst char* e = getenv("SR_GONE");\n#endif\nconst char* k = getenv("SR_KEPT");\n')
    put('kernel/o.cpp', 'const char* o = getenv("SR_ORACLE_UNIT");\n')
    put('kernel/b.hpp', 'inline const char* f() { return getenv("SR_GONE"); }  /* "SR_ORACLE_UNIT" */\n')
    put('tools/difftest/t.cpp', 'const char* t = "SR_ORACLE_UNIT";\n')
    files, oracle, removed, surviving, summaries = gs.sweep(root)
    check('tree: removed = the split-out name and the ORACLE_SOURCES unit\'s name',
          set(removed) == {'SR_GONE', 'SR_ORACLE_UNIT'}, removed)
    check('tree: SR_GONE survives in the header (REL); SR_ORACLE_UNIT only in difftest (NOT-REL), not in a comment',
          surviving == {'SR_GONE': [['kernel/b.hpp', 'REL']], 'SR_ORACLE_UNIT': [['tools/difftest/t.cpp', 'NOT-REL']]},
          surviving)

    def run(*args):
        out, err = io.StringIO(), io.StringIO()
        old = sys.argv
        sys.argv = ['gate_sweep.py'] + list(args)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = gs.main()
        finally:
            sys.argv = old
        return rc, out.getvalue(), err.getvalue()

    rc, out, _ = run('--advisory', '--root', root, '--calls', os.path.join(base, 'calls.txt'))
    check('exit 0 when the sweep runs (survivors listed)', rc == 0 and 'surviving names: 2' in out, out)
    calls = open(os.path.join(base, 'calls.txt'), encoding='utf-8').read()
    check('--calls lists the allowlist and every use', 'defined (operator): 0 use(s)' in calls
          and 'XXH_HAS_INCLUDE (alias of __has_include)' in calls, calls)
    put('kernel/c.hpp', '#define F(x) (x)\n#if F(1)\nint x;\n#endif\n')
    rc, _, err = run('--advisory', '--root', root)
    check('exit 2 on a function-like invocation the allowlist does not hold, naming the file and line',
          rc == 2 and 'kernel/c.hpp:2: F(...)' in err, err)
    put('kernel/c.hpp', '#if SR_ORACLE /* comment */ || 1\nint x;\n#endif\n')
    rc, _, err = run('--advisory', '--root', root)
    check('exit 2 on `#if SR_ORACLE /* c */ || 1` in the tree, naming the file and line',
          rc == 2 and 'kernel/c.hpp:1' in err, err)
    put('kernel/c.hpp', '#if SR_ORACLE \\\n || 1\nint x;\n#endif\n')
    rc, _, err = run('--advisory', '--root', root)
    check('exit 2 on a continued `#if SR_ORACLE \\ || 1` in the tree', rc == 2 and 'kernel/c.hpp:1' in err, err)
    os.remove(os.path.join(root, 'kernel/c.hpp'))
    rc, out, _ = run('--view', os.path.join(root, 'kernel/a.cpp'), '0')
    check('--view 0 prints the release view', rc == 0 and 'SR_GONE' not in out and 'SR_KEPT' in out, out)
    rc, _, err = run('--view', os.path.join(root, 'kernel/a.cpp'), '2')
    check('--view with a value other than 0 / 1 is exit 2', rc == 2, err)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', help="a clang++ to confirm each fixture's clang column on (-E -P -std=c++20)")
    a = ap.parse_args()
    # Run outputs stay off C: (AGENTS.md): the worktree's gitignored archive/ when it exists.
    arch = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'archive')
    base = tempfile.mkdtemp(prefix='gate_sweep_test_', dir=arch if os.path.isdir(arch) else None)
    try:
        unit_checks()
        fixture_checks()
        scan_checks()
        structure_checks(base, a.cc)
        if a.cc:
            clang_checks(a.cc, base)
            clang_mode_checks(a.cc, base)
        else:
            print('skip the clang checks and the clang-view runs (no --cc)')
        tree_checks(base)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('test_gate_sweep: %s (%d failed)' % ('PASS' if not FAILS else 'FAIL', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
