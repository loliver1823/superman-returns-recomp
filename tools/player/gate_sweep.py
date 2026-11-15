#!/usr/bin/env python3
r"""gate_sweep.py - C1 removed-gate sweep, source side: every SR_* environment name the release configuration
(SR_RELEASE_COMPOSITION=ON: SR_ORACLE=0, kernel/sr_oracle.hpp, the ORACLE_SOURCES units dropped) removed, and every
file the release still names it in.

    python tools/player/gate_sweep.py --build build --build-rel build-rel [--uncovered-ok <file>] [--json <out.json>]
           [--jobs N] [--exe SupermanReturns.exe]
    python tools/player/gate_sweep.py --compdb <d.json> --ninja <d/build.ninja> --compdb-rel <r.json> --ninja-rel ...
    python tools/player/gate_sweep.py --advisory [--json ...] [--calls <calls.txt>]            # the lexer alone
    python tools/player/gate_sweep.py --view <file> {0,1}                                       # one lexer view

When an `#if SR_ORACLE` split compiles a CHECK / A/B / oracle out of the release, every other reader of its
environment variable must go too, or a set variable still steers the release (an Astra P2 on the C1 backend split:
a removed replay mode's variables still picked the window policy). The sweep lists the candidates; a person judges
them, and tools/player/exe_sweep.py checks the built release exe for the same names (the ground truth).

The authority - clang's own views (Astra round 7: a lexer that emulates the preprocessor keeps leaking, e.g. an
object-like macro supplying a call and its `(`). Run it where both configurations are built (the cloud worker:
`python tools/cloud/sr_cloud.py run --name <tree> -- python tools/player/gate_sweep.py --build build --build-rel
build-rel --uncovered-ok tools/player/gate_sweep_uncovered.txt ...`).
Inputs: each build's graph (<build>/build.ninja and the rules files it includes, or --ninja) and compile database
  (`ninja -t compdb`, or --compdb).
Units: the release is what the exe links (Astra round 7). In each build's own graph the exe's link closure is derived
  (tools/player/composition_audit.py's Graph.closure: its objects and every static library it links, to a fixpoint
  over link inputs and link bindings - SupermanReturnsGuest, sr_imgui, xenos_xlat today); every C / C++ unit of it
  must have its compile-database entry, for the same source, runnable as tools/pp_receipts.py runs a unit, and
  REQUIRED_TARGETS (SupermanReturns, SupermanReturnsGuest) must each have units in both closures - so an empty or
  partial database is exit 2, and a unit the default links and the release graph does not (an ORACLE_SOURCES unit) has
  an empty SR_ORACLE=0 view because the graph says so, never because an entry is missing. The link commands are read
  too, against a canonical-template allowlist (Astra rounds 8-11: a response file's content, quoted or not, a link
  binding the edge does not hold, ninja's own -x expansion - an unbounded substring swap of @rspfile -, a shell's
  reading of a response file's `&&`, a copy between two links: each could hand the linker an input the closure never
  reads). Each linker edge of the exe (the exe's and each linked static library's) must have one of the command shapes
  the tree's link edges use (LINK_TEMPLATES: CMake's clang++ executable rule through a response file, its llvm-ar rule
  with $in or a response file): its rule's text exactly a template's, the programs aside; every value the template
  reads ninja's own for that edge (`ninja -t compdb` on a scratch copy of the graph whose link rules' command records
  $in, $rspfile, $rspfile_content and each template variable - ninja's scoping, nothing expanded here); the template
  filled with them the command ninja gives the edge (`ninja -t compdb`, never -x); and `ninja -t query` reading the
  edge's rule and inputs as the closure read them. $PRE_LINK must be `cd .`, $POST_BUILD `cd .` or - the exe's -
  CMake's `cmd.exe /C "..."` of `cd /D` and `cmake -E copy* / make_directory` of absolute paths writing no file a
  linker edge reads or writes; no other value on the command line may hold a character cmd.exe reads; the linker's one
  @file must be the edge's rspfile exactly (quoted or not), and the response file's content is split only as linker
  arguments (MSVC's rules, blanks and newlines separating) with no shell operator and no nested @file; every token a
  linker or archiver gets must be an input the edge lists, one of its outputs, a token of the edge's own link bindings
  (LINK_VARS - what the closure reads; a LINK_LIBRARIES bound by the rule or the file is not one) or a flag of
  RULE_FLAGS. Every name the link reads - its command, its response file, its edge's inputs and outputs - is ASCII
  (round 13, Astra: clang drops a response file's leading BOM, the closure read the name with it), no step or link
  argument names a path through a Windows alias (path_alias: \\?\, \\.\, \??\, UNC, an 8.3 name, an alternate data
  stream, a trailing dot or blank), and a post-build copy's destination is compared with the linker edges' files by
  resolved identity (a junction, a hard link). Anything else - another rule shape, a step, a second link, an
  unexpected token, an edge ninja cannot answer for - is exit 2 until a template is added. Each unit is preprocessed
  with its own command - `-E -Xclang -dE`, its outputs stripped, a driver -include made cc1's textual `-Xclang
  -include` (the driver would swap in a .pch), and each -include-pch replaced by the precompiled header's text: the
  PCH must be the output of a compile-database entry that emits it, whose forced includes and source are then included
  where the PCH was (a prebuilt .pch is exit 2), and that text must read in the unit as it read in the PCH (Astra
  round 8: a PCH built with -DSWITCH=1 holds what SWITCH selects and carries SWITCH into the unit) - the PCH's text is
  preprocessed with -E -P -dD (the text without line markers, and every macro, the command line's included) under its
  producer's command, its source the main file, and as the substitution runs it, the unit's command with that source
  force-included, and the two outputs compared exactly, or - when only blanks differ - as tokens read with the -E
  reader (literals whole: Astra round 10, a raw string's line that looks like a marker is the literal's content), else
  exit 2 - a producer -D the unit lacks, an include path that resolves a header elsewhere, text that depends on its
  place (Astra round 9: __INCLUDE_LEVEL__ is 0 in the one, 1 in the other) - once from the default build (the
  SR_ORACLE=1 view) and once from the release build (the SR_ORACLE=0 view). The default database's other units whose
  source is under ROOTS (tools/difftest, ...) are preprocessed in the default view too (NOT-REL).
Names: every SR_* run in the VALUE of a literal in clang's output (Astra round 7) - adjacent string literals
  concatenated (translation phase 6, across line markers and #pragma lines), each piece's escapes decoded (simple,
  octal, \x, \u, \U, \o{}, \x{}, \u{}, \N{}; an unknown simple escape keeps its character, as clang does), a raw
  piece taken as written, the u8 / u / U / L prefixes and a ud-suffix (a whole identifier: non-ASCII characters and
  universal-character-names included, Astra round 8) changing no character, and a run of comma-separated character
  / integer literals (a character array, a generated byte table) read as characters - getenv, env(), gateOn(),
  tables, log text alike, attributed by clang's line markers to the file the value starts in, so headers are covered
  through the units that include them (a build-directory file is `<build>/...`, the same in both views). A lone
  quote, an escape it cannot decode, or a raw string the output does not close is exit 2.
REMOVED = a name of a file's SR_ORACLE=1 view (over all release units) that its SR_ORACLE=0 view (over all release
  units) lacks. SURVIVES = a removed name some file's SR_ORACLE=0 view still has (REL), or a tools/difftest / tests
  file still has (NOT-REL). UNREACHABLE = a file under app/ kernel/ gfx/ guest/ (not guest/ppc) host/ tools/difftest
  that no processed unit enters: the SR_* literals the lexer reads in it are reported, never decided.
UNCOVERED = an input of the exe whose bytes this tool does not examine (Astra rounds 7-8), reported, never skipped: a
  resource script (`resource <file>`, with what it holds) or any other non-C / C++ unit (`unit <file>`), an #embed in
  a release unit (clang's -dE prints the directive instead of its bytes; `embed <file> <directive>`) or an .incbin
  string, a DLL the exe links or delay-loads (`dll <name>`, its units not read), a link input the graph does not build
  (`external <path or library>`; `generated` for a custom command's), a library a release unit asks the linker for -
  `#pragma comment(lib, ...)` (also through _Pragma / __pragma) or `--dependent-lib=` - (`external <library>`, merged
  with the link line's; one the graph builds is covered when the exe links it and exit 2 when it does not), a `#pragma
  comment(linker, ...)` directive (`linker <directive>`); a link step outside the templates is exit 2, never an item
  (round 12). For them tools/player/exe_sweep.py on the built binary (the release exe; a DLL's own file) is the
  authority. --uncovered-ok <file> lists the ones a person has judged (one item per line, exactly as printed; `#`
  lines are comments - put the reason there); tools/player/gate_sweep_uncovered.txt is the tree's list.
Outputs: stdout - `clang views: units N default / N release / N NOT-REL, files entered N, removed names N`, the
  link closure per view (units per component), the units the release graph does not link (checked against
  ORACLE_SOURCES), one line per surviving name `NAME<TAB>removed from <files><TAB>SURVIVES <file> [REL|NOT-REL],
  ...`, `surviving names: N`, the unreachable files with their names, the advisory lexer's result compared with
  clang's, then `UNCOVERED inputs of <exe> (...): N, not acknowledged N` and one line per item
  `  <item><TAB>[views]<TAB><detail><TAB>acknowledged|NOT ACKNOWLEDGED`, and any --uncovered-ok item no longer
  uncovered. --json: {"mode": "clang", "stats" (with "components", "dropped_by_release", "pch_contexts_checked"),
  "removed": {name: [files]}, "surviving": {name: [[file, where], ...]}, "unreachable": {file: [names]},
  "uncovered": [{"item", "views", "detail", "acknowledged"}], "uncovered_authority", "uncovered_not_acknowledged",
  "uncovered_ok_stale", "advisory_lexer"} (exe_sweep.py's --removed input).
Exit: 0 = the views were computed and every UNCOVERED input is acknowledged (survivors, if any, are listed for review
  - the tool does not judge them); 2 = an UNCOVERED input is not acknowledged (the report and the JSON are still
  written), or the views could not be computed: no default or release build graph / compile database, an included
  ninja file holding edges, no edge for the exe, a link input that cannot be read, a linker edge ninja reads
  differently from the closure, a link edge none of LINK_TEMPLATES or whose command - the template filled with ninja's
  values - runs a step outside CMake's known forms or may hand the linker an input the closure does not read (a token
  neither an input, an output, the edge's own link binding nor a known flag; a shell operator or another @file in its
  response file; a non-ASCII name; a Windows path alias), ninja not on PATH or failing for an edge, a linked unit with
  no entry (or an entry for another source), a required target with no unit, a unit that is not a GNU-style compile
  pp_receipts can run, a PCH no entry produces or whose text reads differently in the unit, a compiler that cannot be
  run or cannot show #embed under -dE, a unit that fails to preprocess or whose output holds a literal or #pragma
  comment that cannot be read, a #pragma comment(lib) for a library the graph builds and the exe does not link (each
  named on stderr).

The advisory lexer (--advisory, --view, --calls, and the comparison line of a clang run): fast, needs no build,
never the authority. Each file is read as translation phases 2 and 3 read it (C++ [lex.phases], [lex.pptoken]):
every backslash-newline is spliced, header-names are lexed first where clang lexes them (the operand of #include /
#import / #embed; in an #if-family directive the operand of an `(` - __has_include / __has_embed spelled out or
through an alias - only as a plain path followed by `)`; no escapes), every comment (/* */ and //) becomes one space,
and literals are kept whole - a raw string's delimiter (any d-char, `"` included) and body on the unspliced text, its
splices reverted -, so a directive is a logical line. Each file is read twice, evaluating only conditionals whose
tokens are exactly `#if SR_ORACLE` / `#if ! SR_ORACLE` (`#` or `%:`) with their `#else`; every other conditional keeps
all its lines, and an ORACLE_SOURCES unit's SR_ORACLE=0 view is empty. It fails closed (exit 2 under --advisory; a
note in a clang run) on what it does not model: a function-like invocation in an #if line outside BUILTIN_CALLS /
ALIAS_CALLS (`defined`, `__has_include`, xxhash's XXH_HAS_* aliases, each verified against every #define of it -
--calls writes the allowlist and every use); any other use of the identifier SR_ORACLE (another conditional form,
another directive, code), kernel/sr_oracle.hpp's default definition aside; an #elif or a second #else in a split,
an unbalanced conditional, an unterminated comment, an unterminated or ill-formed raw string, a raw string in any
directive, a string or character literal in a directive naming a macro an #if names, a quoted operand that reads
differently as a header-name and as a string literal, an unterminated header-name, an `(<` operand in an #if line
that is not a plain path ([A-Za-z0-9_./+-]) followed by `)`, a header-name holding SR_ORACLE or an #if macro's name,
a non-ASCII character outside comments and literals. Even so it cannot see through every macro (an object-like macro
can supply a call and its `(`), which is why it only advises. Advisory outputs: `advisory (lexer, not the
authority): files N, ORACLE_SOURCES N, removed names N`, then the same survivor lines (REL = a header, an
APP_SOURCES unit or a guest/ unit; NOT-REL = tools/difftest or tests; OTHER = neither).
Tests: tools/player/test_gate_sweep.py [--cc <clang++>] (the literal reader, the closure and database checks and the
  PCH resolution run without a compiler; --cc adds the clang views on a scratch tree).
Limits: a name code assembles - at run time, or by a constexpr / template computation, or spelled as characters
  through casts or expressions rather than a plain comma-separated run of literals - is in no literal value (the
  exe scan sees what the binary holds); the receipts (tools/pp_receipts.py) remain the proof of what each
  configuration compiles.
"""
import argparse
import bisect
import collections
import concurrent.futures
import functools
import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import unicodedata

EXT = ('.cpp', '.hpp', '.h', '.inc', '.inl')
ROOTS = ('app', 'kernel', 'gfx', 'guest', 'host', 'tools/difftest')
NAME = re.compile(r'SR_[A-Z0-9_]+')    # found anywhere in a literal's value (log text, a table, a getenv argument)
STR_BODY = re.compile(r'(?:u8|u|U|L)?"((?:[^"\\\n]|\\.)*)')
# Translation phase 2: a backslash, optional blanks (clang and gcc accept them, with a warning), then a newline.
SPLICE = re.compile(r'\\[ \t\f\v]*\n')
# Phase 3, one preprocessing token (or comment, or blank run) at a time, on the spliced text. Literal prefixes are
# taken only where an identifier starts (`id` consumes whole ASCII identifiers; a non-ASCII character outside comments
# and literals is exit 2, so `eR"(` with a non-ASCII e never reaches here as `R"`); a pp-number swallows its digit
# separators (1'000), so their quote never opens a character literal. `raw` is only the prefix and the opening quote:
# a raw string's delimiter and body are read on the UNSPLICED text ([lex.pptoken]: its splices are reverted).
TOKEN = re.compile(r'''
    (?P<bc>/\*)
  | (?P<lc>//[^\n]*)
  | (?P<raw>(?:u8|u|U|L)?R")
  | (?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"?)
  | (?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'?)
  | (?P<num>\.?[0-9](?:'[0-9A-Za-z_]|[eEpP][+-]|[0-9A-Za-z_.])*)
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<nl>\n)
  | (?P<ws>[ \t\f\v\r]+)
  | (?P<punct>.)
''', re.X | re.S)
# A raw string's d-char-sequence: up to 16 of any basic character except space, ( ) \ and the four blank controls -
# `"` included (R""(x)"" is a raw string with the delimiter `"`). Then the `(`.
RAW_OPEN = re.compile(r'([^ ()\\\t\v\f\n]{0,16})\(')
CONDITIONALS = ('if', 'ifdef', 'ifndef', 'elif', 'elifdef', 'elifndef')
# Header-names ([lex.header]) are lexed before comments and literals, where clang lexes them: the operand of
# #include / #include_next / #import / #embed, and - in an #if-family directive - the operand of an `(` (spelled-out
# __has_include / __has_include_next / __has_embed, or an alias such as xxhash's XXH_HAS_INCLUDE, whose operand clang
# reads from the file as a header-name). A q-char or h-char sequence has no escapes, so `//`, `/*`, `"`, `'` and `\`
# inside one are characters, not comment, literal or escape starts. In an #if line `(<` is not always a header-name:
# for an ordinary function-like macro it is a macro argument (`#define F(x) (0 x 0)` + `#if F(< SR_ORACLE >)` is
# `(0 < SR_ORACLE > 0)`), so there only a plain path read whole and followed by `)` - <arm_neon.h> - is a
# header-name; any other `(<` operand in an #if line is exit 2.
HEADER_INCLUDES = ('include', 'include_next', 'import', 'embed')
HAS_OPS = ('__has_include', '__has_include_next', '__has_embed')
H_NAME = re.compile(r'<[^>\n]*>')
Q_NAME = re.compile(r'"[^"\n]*"')
IF_H_NAME = re.compile(r'<[A-Za-z0-9_./+-]+>(?=[ \t]*\))')   # a plain path, then `)`
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


def header_context(cur):
    """Where the next `<` / `"` of the logical line `cur` (its tokens so far) starts a header-name: 'include' (an
    #include-family operand), 'has' (right after a spelled-out __has_include `(`), 'paren' (after another `(` in an
    #if-family directive: a header-name if the macro is a __has_include alias, else a literal), or None."""
    toks = [t for t in cur if t[0] != 'ws']
    if toks[:1] == [('punct', '#')]:
        rest = toks[1:]
    elif toks[:2] == [('punct', '%'), ('punct', ':')]:
        rest = toks[2:]
    else:
        return None
    if not rest or rest[0][0] != 'id':
        return None
    if rest[0][1] in HEADER_INCLUDES and len(rest) == 1:
        return 'include'
    if rest[0][1] in CONDITIONALS and len(rest) >= 2 and rest[-1] == ('punct', '('):
        return 'has' if rest[-2][0] == 'id' and rest[-2][1] in HAS_OPS else 'paren'
    return None
SUPPORTED = {('SR_ORACLE',): 1, ('!', 'SR_ORACLE'): 0}   # `#if <tokens>` -> the SR_ORACLE value whose body it keeps


class Unmodelled(Exception):
    pass


def default_definition(name, kw, toks):
    """kernel/sr_oracle.hpp defines the default (`#ifndef SR_ORACLE` / `#define SR_ORACLE 1`): not a split."""
    texts = [t for _, t in toks]
    return name.replace(os.sep, '/').endswith('kernel/sr_oracle.hpp') and (
        (kw == 'ifndef' and texts == ['SR_ORACLE']) or (kw == 'define' and texts == ['SR_ORACLE', '1']))


def logical_lines(text, name='<text>'):
    """The text after translation phases 2 and 3 (C++ [lex.phases]): every backslash-newline spliced, every comment
    one blank, literals whole (a raw string's delimiter and body read on the unspliced text: the splices between its
    quotes are reverted). Returns [(first, last, tokens)]: one entry per logical line, `first`..`last` the 0-based
    physical lines it covers (the entries partition text.split('\\n')), `tokens` its [(kind, text)] - kind one of
    ws (blanks and comments), str, chr, raw, num, id, punct. Raises Unmodelled on a non-ASCII character outside
    comments and literals (a leading byte-order mark aside), an unterminated comment, or an unterminated or ill-formed
    raw string."""
    if text.startswith('﻿'):
        text = ' ' + text[1:]
    pieces, s_at, o_at, cum = [], [], [], []   # per splice: spliced offset, original offset, characters removed so far
    at = size = total = 0
    for m in SPLICE.finditer(text):
        pieces.append(text[at:m.start()])
        size += m.start() - at
        total += m.end() - m.start()
        s_at.append(size)
        o_at.append(m.start())
        cum.append(total)
        at = m.end()
    pieces.append(text[at:])
    src = ''.join(pieces)
    newlines = [i for i, c in enumerate(text) if c == '\n']

    def to_orig(si):
        k = bisect.bisect_right(s_at, si)
        return si + (cum[k - 1] if k else 0)

    def to_spliced(oi):  # oi must not fall inside a splice
        k = bisect.bisect_left(o_at, oi)
        return oi - (cum[k - 1] if k else 0)

    def phys(si):
        return bisect.bisect_left(newlines, to_orig(si))

    out, cur, first, pos = [], [], 0, 0
    while pos < len(src):
        if src[pos] in '<"' and cur:
            hk = header_context(cur)
            if hk:
                hm = (H_NAME if src[pos] == '<' else Q_NAME).match(src, pos)
                if src[pos] == '"':
                    # A quoted operand: a header-name has no escapes, a string literal has (and clang reads
                    # __has_include("no\") as an unterminated string) - the two readings must end at the same quote,
                    # or the operand is ambiguous. After `F(` in an #if it stays a string literal (F may be an
                    # ordinary macro); elsewhere it is the header-name.
                    sm = TOKEN.match(src, pos)
                    if not hm or sm.group(0) != hm.group(0):
                        raise Unmodelled(f'{name}:{phys(pos) + 1}: a quoted operand that reads differently as a '
                                         f'header-name and as a string literal')
                if src[pos] == '<' and hk != 'include':
                    hm = IF_H_NAME.match(src, pos)
                    if not hm:
                        raise Unmodelled(f'{name}:{phys(pos) + 1}: an `(<` operand in an #if-family directive that is '
                                         f'not a plain-path header-name followed by `)` (it may be a macro argument)')
                if src[pos] == '<' or hk != 'paren':
                    if not hm:
                        raise Unmodelled(f'{name}:{phys(pos) + 1}: unterminated header-name')
                    cur.append(('hdr', hm.group(0)))
                    pos = hm.end()
                    continue
        m = TOKEN.match(src, pos)
        kind = m.lastgroup
        if kind == 'bc':
            end = src.find('*/', m.end())
            if end < 0:
                raise Unmodelled(f'{name}:{phys(pos) + 1}: unterminated /* comment')
            cur.append(('ws', ' '))
            pos = end + 2
            continue
        if kind == 'raw':
            quote = to_orig(m.end() - 1)               # the opening quote, in the unspliced text
            d = RAW_OPEN.match(text, quote + 1)
            if not d:
                raise Unmodelled(f'{name}:{phys(pos) + 1}: ill-formed raw string literal (no delimiter and `(` within '
                                 f'16 characters - a reverted splice there counts)')
            close = ')' + d.group(1) + '"'
            end = text.find(close, d.end())
            if end < 0:
                raise Unmodelled(f'{name}:{phys(pos) + 1}: unterminated raw string literal')
            end += len(close)
            cur.append(('raw', src[pos:m.end()] + text[quote + 1:end]))
            pos = to_spliced(end)
            continue
        if kind == 'lc':
            cur.append(('ws', ' '))
        elif kind == 'nl':
            last = phys(pos)
            out.append((first, last, cur))
            cur, first = [], last + 1
        elif kind == 'punct' and ord(m.group(0)) > 127:
            raise Unmodelled(f'{name}:{phys(pos) + 1}: non-ASCII character U+{ord(m.group(0)):04X} outside comments '
                             f'and literals')
        else:
            cur.append((kind, m.group(0)))
        pos = m.end()
    out.append((first, text.count('\n'), cur))
    return out


def directive(tokens):
    """(keyword, [(kind, text)] after it, blanks dropped) of a directive line (`#`, or its digraph `%:` written as two
    adjacent tokens), keyword '' for a null directive; None for any other line."""
    toks = list(tokens)
    while toks and toks[0][0] == 'ws':
        toks.pop(0)
    if toks[:1] == [('punct', '#')]:
        rest = toks[1:]
    elif toks[:2] == [('punct', '%'), ('punct', ':')]:
        rest = toks[2:]
    else:
        return None
    rest = [t for t in rest if t[0] != 'ws']
    if rest and rest[0][0] == 'id':
        return rest[0][1], rest[1:]
    return '', rest


def names_sr_oracle(toks):
    return ('id', 'SR_ORACLE') in toks   # the identifier; a literal "SR_ORACLE" is a str token


OPERATORS = {'defined', *HAS_OPS}   # #if operators, not macros
# The function-like forms an #if-family expression may invoke (an identifier followed by `(`): exactly those this tree
# uses (tools/player/gate_sweep.py --calls lists every use), each a clang operator / builtin, or a project alias that
# every definition of in the swept files resolves to one: `#define A B`, `#define A(x) B(x)`, or a constant fallback
# `#define A(x) 0` (the argument unused). ANY other invocation in an #if line is exit 2 - so no macro argument, shaped
# like a header-name or not, can hide a token (Astra round 6: `#if F(<X>)` with X defined differently per SR_ORACLE).
# A new form is added here deliberately, never by widening a pattern.
BUILTIN_CALLS = {'defined': 'operator', '__has_include': 'clang builtin'}
ALIAS_CALLS = {   # gfx/thirdparty/xxhash.h's feature-test wrappers
    'XXH_HAS_ATTRIBUTE': '__has_attribute',
    'XXH_HAS_BUILTIN': '__has_builtin',
    'XXH_HAS_CPP_ATTRIBUTE': '__has_cpp_attribute',
    'XXH_HAS_C_ATTRIBUTE': '__has_c_attribute',
    'XXH_HAS_INCLUDE': '__has_include',
}
Directive = collections.namedtuple('Directive', 'line kw ids has_raw has_lit hdr_ids calls define')


def define_record(tokens):
    """(name, 'object' | 'function', params, body texts) of a #define line's tokens (blanks kept: a function-like
    macro's `(` follows its name with no blank); None if the line names no macro."""
    toks = [t for t in tokens]
    i = 0
    while i < len(toks) and toks[i][0] == 'ws':
        i += 1
    i += 1 if toks[i] == ('punct', '#') else 2
    while i < len(toks) and toks[i][0] == 'ws':
        i += 1
    i += 1                                             # the `define` keyword
    while i < len(toks) and toks[i][0] == 'ws':
        i += 1
    if i >= len(toks) or toks[i][0] != 'id':
        return None
    name, i = toks[i][1], i + 1
    if i < len(toks) and toks[i] == ('punct', '('):
        j, params = i + 1, []
        while j < len(toks) and toks[j] != ('punct', ')'):
            if toks[j][0] == 'id':
                params.append(toks[j][1])
            j += 1
        return name, 'function', tuple(params), tuple(t for k, t in toks[j + 1:] if k != 'ws')
    return name, 'object', (), tuple(t for k, t in toks[i:] if k != 'ws')


def directive_summary(logical):
    """A Directive per directive line: its keyword, the identifiers after it, whether it holds a raw string / a
    string or character literal, the identifiers inside its header-names (a header-name - #include "x" / <x>,
    __has_include("x") / (<x>) - is not a literal), the function-like invocations of a conditional (an identifier
    followed by `(`), and a #define's record."""
    out = []
    for first, _, tokens in logical:
        d = directive(tokens)
        if d:
            kw, toks = d
            calls = [t for i, (k, t) in enumerate(toks[:-1]) if k == 'id' and toks[i + 1] == ('punct', '(')] \
                if kw in CONDITIONALS else []
            out.append(Directive(first, kw, {t for k, t in toks if k == 'id'}, any(k == 'raw' for k, _ in toks),
                                 any(k in ('str', 'chr') for k, _ in toks),
                                 {i for k, t in toks if k == 'hdr' for i in IDENT.findall(t[1:-1])}, calls,
                                 define_record(tokens) if kw == 'define' else None))
    return out


def alias_problem(alias, defs):
    """Why `alias` (ALIAS_CALLS) does not resolve to its builtin over its #define records [(file, line, record)], or
    None when every definition is `#define A B`, `#define A(x) B(x)` or `#define A(x) <number>`."""
    target = ALIAS_CALLS[alias]
    if not defs:
        return 'no #define of it in the swept files'
    for f, line, (_, form, params, body) in defs:
        ok = (form == 'object' and body == (target,)) or (
            form == 'function' and len(params) == 1 and (body == (target, '(', params[0], ')') or (
                len(body) == 1 and body[0][:1].isdigit())))
        if not ok:
            return f'{f}:{line}: #define {alias} is not {target} or a constant fallback'
    return None


def check_directives(summaries):
    """Fail closed on the directive forms a textual view cannot be trusted with, over {name: directive_summary}: a
    function-like invocation in a conditional that BUILTIN_CALLS / ALIAS_CALLS do not allow (or an alias some
    definition of does not resolve); a raw string literal in any directive; a string or character literal in any
    directive that names (any identifier after its keyword) a macro some #if / #elif / #ifdef / ... expression in
    these files names; a header-name whose text holds SR_ORACLE or such a macro's name."""
    in_if, defs = set(), collections.defaultdict(list)
    for name, summary in summaries.items():
        for d in summary:
            if d.kw in CONDITIONALS:
                in_if |= d.ids - OPERATORS
            if d.define:
                defs[d.define[0]].append((name, d.line + 1, d.define))
    for name, summary in summaries.items():
        for d in summary:
            where = f'{name}:{d.line + 1}'
            for c in d.calls:
                if c in BUILTIN_CALLS:
                    continue
                if c not in ALIAS_CALLS:
                    raise Unmodelled(f'{where}: {c}(...) in a #{d.kw} directive - a function-like invocation the '
                                     f'allowlist does not hold (its arguments could hide a token)')
                problem = alias_problem(c, defs.get(c, []))
                if problem:
                    raise Unmodelled(f'{where}: {c}(...) in a #{d.kw} directive does not resolve to '
                                     f'{ALIAS_CALLS[c]}: {problem}')
            if d.has_raw:
                raise Unmodelled(f'{where}: a raw string literal in a #{d.kw} directive')
            named = sorted(d.ids & in_if)
            if named and d.has_lit:
                raise Unmodelled(f'{where}: a string / character literal in a #{d.kw} directive naming '
                                 f'{", ".join(named)}, which an #if expression names')
            inside = sorted(d.hdr_ids & (in_if | {'SR_ORACLE'}))
            if inside:
                raise Unmodelled(f'{where}: a header-name in a #{d.kw} directive holding '
                                 f'{", ".join(inside)} (SR_ORACLE or a macro an #if expression names)')


def call_report(summaries):
    """The allowlist and where each entry is used (and, for an alias, defined), as text lines."""
    uses, defs = collections.defaultdict(list), collections.defaultdict(list)
    for name, summary in summaries.items():
        for d in summary:
            for c in d.calls:
                uses[c].append(f'{name}:{d.line + 1}')
            if d.define and d.define[0] in ALIAS_CALLS:
                _, form, params, body = d.define
                defs[d.define[0]].append(f'{name}:{d.line + 1} #define {d.define[0]}'
                                         f'{"(" + ", ".join(params) + ")" if form == "function" else ""} {" ".join(body)}')
    out = ['allowed function-like forms in #if-family expressions (BUILTIN_CALLS / ALIAS_CALLS in gate_sweep.py); any '
           'other identifier followed by `(` in an #if line is exit 2']
    for c, what in BUILTIN_CALLS.items():
        out.append(f'{c} ({what}): {len(uses[c])} use(s)')
        out += [f'    {u}' for u in uses[c]]
    for c, target in ALIAS_CALLS.items():
        out.append(f'{c} (alias of {target}): {len(uses[c])} use(s); definitions:')
        out += [f'    def {x}' for x in defs[c]]
        out += [f'    use {u}' for u in uses[c]]
    unused = [c for c in (*BUILTIN_CALLS, *ALIAS_CALLS) if not uses[c]]
    out.append(f'entries with no use in the swept files: {", ".join(unused) or "none"}')
    return out


def select(logical, v, name='<text>'):
    """Which logical lines SR_ORACLE=<v> keeps: only `#if SR_ORACLE` / `#if !SR_ORACLE` conditionals (exactly those
    tokens) and their #else are evaluated; every other conditional keeps all its lines. Raises Unmodelled on any
    other mention of the identifier SR_ORACLE - in a conditional (`#if SR_ORACLE and 1`, `#if not SR_ORACLE`,
    `defined`, `#ifdef`, ...), in another directive (`#define X SR_ORACLE`, `#undef`) or in code (`if constexpr
    (SR_ORACLE)`) - kernel/sr_oracle.hpp's default definition aside; on an #elif in or on a split, a second #else in
    one, or an unbalanced conditional."""
    keep, stack = [], []  # stack entries: [is_oracle, active, seen_else]
    for idx, (first, _, tokens) in enumerate(logical):
        where = f'{name}:{first + 1}'
        d = directive(tokens)
        if d is None:
            if names_sr_oracle(tokens):
                raise Unmodelled(f'{where}: SR_ORACLE used outside an #if SR_ORACLE / #if !SR_ORACLE: '
                                 f'{"".join(t for _, t in tokens).strip()[:80]}')
        else:
            kw, toks = d
            texts = tuple(t for _, t in toks)
            if kw in ('if', 'ifdef', 'ifndef'):
                ora = kw == 'if' and texts in SUPPORTED and toks[-1] == ('id', 'SR_ORACLE')
                if not ora and names_sr_oracle(toks) and not default_definition(name, kw, toks):
                    raise Unmodelled(f'{where}: unmodelled SR_ORACLE conditional: #{kw} {" ".join(texts)}')
                stack.append([ora, (SUPPORTED[texts] == v) if ora else True, False])
                if ora:
                    continue
            elif kw in ('elif', 'elifdef', 'elifndef'):
                if not stack:
                    raise Unmodelled(f'{where}: #{kw} without #if')
                if stack[-1][0] or names_sr_oracle(toks):
                    raise Unmodelled(f'{where}: #{kw} in or on an SR_ORACLE conditional: #{kw} {" ".join(texts)}')
            elif kw == 'else':
                if not stack:
                    raise Unmodelled(f'{where}: #else without #if')
                if stack[-1][0]:
                    if stack[-1][2]:
                        raise Unmodelled(f'{where}: second #else in an SR_ORACLE conditional')
                    stack[-1][1], stack[-1][2] = not stack[-1][1], True
                    continue
            elif kw == 'endif':
                if not stack:
                    raise Unmodelled(f'{where}: #endif without #if')
                if stack.pop()[0]:
                    continue
            elif names_sr_oracle(toks) and not default_definition(name, kw, toks):
                raise Unmodelled(f'{where}: SR_ORACLE in a #{kw} directive: #{kw} {" ".join(texts)}')
        if any(o and not active for o, active, _ in stack):
            continue
        keep.append(idx)
    if stack:
        raise Unmodelled(f'{name}: unbalanced conditional ({len(stack)} open at end of file)')
    return keep


def literal_names(tokens):
    """The SR_* runs in the string literals (raw included) among `tokens`, as spelled - the lexer's own spans (it
    neither concatenates nor decodes: clang's views do)."""
    out = set()
    for kind, t in tokens:
        if kind == 'str':
            body = STR_BODY.match(t).group(1)
        elif kind == 'raw':
            body = t[t.index('(') + 1:t.rindex(')')] if ')' in t else t[t.index('(') + 1:]
        else:
            continue
        out.update(m.group(0) for m in NAME.finditer(body))
    return out


def view(text, v, name='<text>'):
    """The text as SR_ORACLE=<v> sees it (its kept physical lines, as written)."""
    lines = text.split('\n')
    logical = logical_lines(text, name)
    check_directives({name: directive_summary(logical)})
    return '\n'.join(l for i in select(logical, v, name) for l in lines[logical[i][0]:logical[i][1] + 1])


def view_names(text, name='<text>', logical=None):
    """{v: the SR_* names string literals start with in the SR_ORACLE=<v> view} for v = 1 and 0. With `logical`
    given (the sweep), the directive check is the caller's, over every file at once."""
    if logical is None:
        logical = logical_lines(text, name)
        check_directives({name: directive_summary(logical)})
    return {v: set().union(*(literal_names(logical[i][2]) for i in select(logical, v, name))) for v in (1, 0)}


def cmake_list(cm, var):
    m = re.search(r'set\(' + var + r'(.*?)\n\)', cm, re.S)
    if not m:
        raise Unmodelled(f'CMakeLists.txt: no set({var} ...) block')
    return {l.split('#')[0].strip() for l in m.group(1).splitlines() if l.split('#')[0].strip()}


def read(path):
    return open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')


def sweep(root):
    cm = read(os.path.join(root, 'CMakeLists.txt'))
    oracle, app = cmake_list(cm, 'ORACLE_SOURCES'), cmake_list(cm, 'APP_SOURCES')
    files = scan_files(root)
    v1, v0, summaries = {}, {}, {}
    for f in files:
        logical = logical_lines(read(os.path.join(root, f)), f)
        summaries[f] = directive_summary(logical)
        both = view_names(None, f, logical)
        v1[f] = both[1]
        v0[f] = set() if f in oracle else both[0]
    check_directives(summaries)
    removed = {}
    for f in files:
        for n in v1[f] - v0[f]:
            removed.setdefault(n, []).append(f)

    def where(f):
        if f.startswith(('tools/difftest/', 'tests/')):
            return 'NOT-REL'
        if not f.endswith(('.cpp', '.cc')) or f in app or f.startswith('guest/'):
            return 'REL'
        return 'OTHER'

    surviving = {n: [[f, where(f)] for f in files if n in v0[f]] for n in sorted(removed)}
    surviving = {n: v for n, v in surviving.items() if v}
    return files, oracle, {n: sorted(v) for n, v in sorted(removed.items())}, surviving, summaries


# ---------------------------------------------------------------------------------------------------------------------
# The authority: the two views computed by clang itself (Astra round 7 - a lexer that emulates the preprocessor keeps
# leaking: object-like macros could supply a call and its `(`). The units are the release exe's link closure in each
# build's own graph (Astra round 8: two hard-coded targets missed sr_imgui and xenos_xlat, and only the graph can tell
# an ORACLE_SOURCES unit the release drops from a compile-database entry that is missing), each preprocessed with its
# own compile command (`-E -Xclang -dE`, its outputs stripped - as tools/pp_receipts.py does), once from the default
# build (SR_ORACLE=1) and once from the release build (SR_RELEASE_COMPOSITION=ON: SR_ORACLE=0). The names are the SR_*
# runs in the VALUES of clang's literals - adjacent string literals concatenated, escapes decoded, character / integer
# runs read as characters - attributed by its line markers to the file each value starts in.
# ---------------------------------------------------------------------------------------------------------------------
RELEASE_EXE = 'SupermanReturns.exe'                              # the release exe: its link closure is the release
REQUIRED_TARGETS = ('SupermanReturns', 'SupermanReturnsGuest')   # the closure must hold units of both, in both builds
NOT_REL_DIRS = ('tools/difftest/', 'tests/')                     # never in the release
TARGET_DIR = re.compile(r'(?:^|/)CMakeFiles/([^/]+)\.dir/')
C_RULES = ('CXX_COMPILER__', 'C_COMPILER__')                     # CMake's C / C++ compile rules: preprocessed here
UNCOVERED_AUTHORITY = ("not examined by this tool - tools/player/exe_sweep.py on the built binary (the release exe; "
                       "a DLL's own file) is the authority for them")
# Tokens of clang's -E output (no comments; directives only as line markers, #pragma / #ident lines and, under
# -Xclang -dE, #embed lines): directive lines, raw strings, strings, character literals (each with its encoding
# prefix - taken only where an identifier starts - and a ud-suffix: a whole identifier, non-ASCII characters and
# universal-character-names included, so `"SR_"_é "LIVE"` still concatenates), pp-numbers (digit separators),
# identifiers (whole, likewise), blanks, a newline, commas, a lone quote (exit 2), anything else.
ID_CHAR = r'(?:[A-Za-z0-9_]|[^\x00-\x7f]|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8}|\\u\{[^}\n]*\}|\\N\{[^}\n]*\})'
ID_START = r'(?:[A-Za-z_]|[^\x00-\x7f]|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8}|\\u\{[^}\n]*\}|\\N\{[^}\n]*\})'
NOT_AFTER_ID = r'(?<![A-Za-z0-9_\u0080-\U0010ffff])'   # an identifier's last character never precedes a token here
UD = '(?:' + ID_START + ID_CHAR + '*)?'
LIT_PREFIX = '(?:' + NOT_AFTER_ID + '(?:u8|u|U|L))?'
OUT_TOKEN = re.compile('|'.join((
    r'(?P<directive>^[ \t]*#[^\n]*)',
    r'(?P<raw>' + NOT_AFTER_ID + r'(?:u8|u|U|L)?R"(?P<rd>[^ ()\\\t\v\f\n]{0,16})\((?P<rbody>.*?)\)(?P=rd)"' + UD + ')',
    r'(?P<str>' + LIT_PREFIX + r'"(?P<sbody>(?:[^"\\\n]|\\.)*)"' + UD + ')',
    r"(?P<chr>" + LIT_PREFIX + r"'(?P<cbody>(?:[^'\\\n]|\\.)*)'" + UD + ')',
    r"(?P<num>" + NOT_AFTER_ID + r"\.?[0-9](?:'[0-9A-Za-z_]|[eEpP][+-]|[0-9A-Za-z_.])*)",
    r'(?P<id>' + ID_START + ID_CHAR + '*)',
    r'(?P<ws>[ \t\r\f\v]+|\n)',
    r'(?P<comma>,)',
    r'''(?P<quote>["'])''',
    r'''(?P<other>[^"'A-Za-z0-9_,\s.\\\u0080-\U0010ffff]+|.)''',
)), re.S | re.M)
# `#pragma comment(lib, "x")` / `(linker, "...")` (also from _Pragma and __pragma, which -E prints as #pragma): the
# object asks the linker for an input (/DEFAULTLIB) or passes it a directive - an input of the exe like any other.
PRAGMA_COMMENT = re.compile(r'[ \t]*#[ \t]*pragma[ \t]+comment[ \t]*\([ \t]*([A-Za-z_]+)[ \t]*(?:,(.*))?\)[ \t]*$')
PRAGMA_COMMENT_START = re.compile(r'[ \t]*#[ \t]*pragma[ \t]+comment\b')
MARKER = re.compile(r'[ \t]*#[ \t]*(?:line[ \t]+)?\d+(?:[ \t]+"(?P<mfile>(?:\\.|[^"\\\n])*)")?')
EMBED_DIR = re.compile(r'[ \t]*#[ \t]*embed\b')
RAW_IDS = ('R', 'u8R', 'uR', 'UR', 'LR')     # an identifier clang would have lexed as a raw string's start
INCBIN = re.compile(r'\.incbin\b', re.I)     # an assembler directive that embeds a file's bytes
INT_LIT = re.compile(r"(?:0[xX](?P<x>[0-9A-Fa-f']+)|0[bB](?P<b>[01']+)|(?P<o>0[0-7']*)|(?P<d>[1-9][0-9']*))"
                     r"(?:[uU](?:ll|LL|l|L|z|Z)?|(?:ll|LL|l|L|z|Z)[uU]?)?")
# Escape sequences ([lex.ccon], C++23 delimited and named forms included); an unknown simple escape keeps its
# character, as clang does (-Wunknown-escape-sequence). Anything else - \x with no digit, \u / \U short of digits,
# a bad delimited sequence, a \N{} name Python's Unicode database does not know - is exit 2.
ESCAPE = re.compile(r'''\\(?:
    (?P<oct>[0-7]{1,3})
  | o\{(?P<oct_d>[^}]*)\}
  | x\{(?P<hex_d>[^}]*)\}
  | x(?P<hex>[0-9A-Fa-f]+)
  | u\{(?P<ucn_d>[^}]*)\}
  | u(?P<u4>[0-9A-Fa-f]{4})
  | U(?P<u8>[0-9A-Fa-f]{8})
  | N\{(?P<named>[^}]*)\}
  | (?P<char>[\s\S])
)''', re.X)
SIMPLE_ESCAPES = {"'": "'", '"': '"', '?': '?', '\\': '\\', 'a': '\a', 'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r',
                  't': '\t', 'v': '\v', 'e': '\x1b', 'E': '\x1b'}


def decode_escapes(body):
    """A string or character literal's body with its escape sequences decoded (Unmodelled on one it cannot)."""
    def one(m):
        g = m.lastgroup
        if g == 'char':
            c = m.group('char')
            if c in SIMPLE_ESCAPES:
                return SIMPLE_ESCAPES[c]
            if c in 'xuUoN' or c == '\n':
                raise Unmodelled(f'an escape sequence that cannot be decoded: {m.group(0)!r}')
            return c
        if g == 'named':
            try:
                return unicodedata.lookup(m.group('named'))
            except KeyError:
                raise Unmodelled(f'a named escape this tool cannot decode: {m.group(0)!r}') from None
        digits = m.group(g)
        if g in ('oct_d', 'hex_d', 'ucn_d') and not re.fullmatch(r'[0-7]+' if g == 'oct_d' else r'[0-9A-Fa-f]+',
                                                                   digits):
            raise Unmodelled(f'an escape sequence that cannot be decoded: {m.group(0)!r}')
        v = int(digits, 8 if g in ('oct', 'oct_d') else 16)
        # A code unit past U+10FFFF (fmt's U"\x9999999a..." tables: a char32_t holds any 32-bit value) is no character
        # of a name. A value past its element type's range is a compile error (clang: "escape sequence out of
        # range"), so a unit that builds never wraps one into an ASCII character.
        return chr(v) if v <= 0x10FFFF else '�'
    return ESCAPE.sub(one, body)


def int_value(t):
    """An integer literal's value, or None (a floating literal, a ud-suffix, ...)."""
    m = INT_LIT.fullmatch(t)
    if not m:
        return None
    for g, base in (('x', 16), ('b', 2), ('o', 8), ('d', 10)):
        if m.group(g) is not None:
            return int(m.group(g).replace("'", '') or '0', base)
    return None


@functools.lru_cache(maxsize=None)
def unit_path(path, root, build=None):
    """A file as the views name it: `<build>/...` under the unit's build directory (both builds' generated files and
    precompiled-header stubs read as one file), repo-relative under root (`/` separators), else its full path."""
    full = os.path.realpath(path).replace('\\', '/')
    for base, label in ((build, '<build>/'), (root, '')):
        if base:
            b = os.path.realpath(base).replace('\\', '/').rstrip('/') + '/'
            if os.path.normcase(full).startswith(os.path.normcase(b)):
                return label + full[len(b):]
    return full


def repo_path(path, root):
    """A file's path relative to root (on-disk case, `/` separators) when it is under root, else its full path."""
    return unit_path(path, root)


def output_scan(text, directory, root, build=None):
    """({file: SR_* names}, files entered, [(file, kind, what)]) of one -E output (clang's, with -dE): the inputs
    are each #embed ('embed', the directive), .incbin string ('incbin') and `#pragma comment(lib | linker, ...)`
    ('lib', the library; 'linker', the directive) - an object's own requests of the linker (Astra round 8).
    A name is any SR_* run in a literal's VALUE (Astra round 8: "SR_" "LIVE", "SR_" #x and "SR_\\x4cIVE" are all
    SR_LIVE): adjacent string literals are concatenated (translation phase 6: across blanks, newlines, line markers
    and #pragma lines), each piece's escapes decoded (a raw string's body taken as written; the u8 / u / U / L prefixes
    and a ud-suffix - a whole identifier, non-ASCII and universal-character-names included - change no character), and
    a run of comma-separated character / integer literals (an array spelled as bytes, a generated byte table) is read
    as characters. A value is attributed to the file its first token is in.
    Unmodelled on a lone quote, an escape it cannot decode, a raw string clang's output does not close, or a
    `#pragma comment` it cannot read."""
    names, entered, embeds = collections.defaultdict(set), set(), []
    state = {'cur': '<before any line marker>'}

    def fail(m, why):
        raise Unmodelled(f'{why} at line {text.count(chr(10), 0, m.start()) + 1} of the -E output '
                         f'(in {state["cur"]}): {text[m.start():m.start() + 60]!r}')

    def region(start, end):
        lit_file, lit = None, []                         # adjacent string literals: one value
        run_file, run, want_value = None, [], True       # comma-separated character / integer literals

        def flush_lit():
            nonlocal lit_file, lit
            if lit:
                value = ''.join(lit)
                names[lit_file].update(n.group(0) for n in NAME.finditer(value))
                if INCBIN.search(value):
                    embeds.append((lit_file, 'incbin', '.incbin (a string literal holding an assembler embed)'))
            lit_file, lit = None, []

        def flush_run():
            nonlocal run_file, run, want_value
            if len(run) >= 4:
                value = ''.join(chr(c) if 0 <= c <= 0x10FFFF else '\0' for c in run)
                names[run_file].update(n.group(0) for n in NAME.finditer(value))
            run_file, run, want_value = None, [], True

        for m in OUT_TOKEN.finditer(text, start, end):
            g = m.lastgroup
            if g == 'ws':
                continue
            if g == 'directive':                          # only at a line start, so never inside a directive
                line = m.group(0)
                mk = MARKER.match(line)
                if mk:
                    if mk.group('mfile') is not None:
                        p = re.sub(r'\\(.)', r'\1', mk.group('mfile'))
                        if p.startswith('<'):             # <built-in>, <command line>
                            state['cur'] = p
                        else:
                            state['cur'] = unit_path(os.path.join(directory, p), root, build)
                            entered.add(state['cur'])
                    continue
                if EMBED_DIR.match(line):                 # -Xclang -dE: the directive, not its bytes
                    flush_lit()
                    flush_run()
                    embeds.append((state['cur'], 'embed', re.sub(r'\s*/\*.*?\*/\s*$', '', line.strip())))
                    continue
                if PRAGMA_COMMENT_START.match(line):
                    pc = PRAGMA_COMMENT.match(line)
                    if not pc:
                        fail(m, 'a #pragma comment this tool does not read')
                    if pc.group(1) in ('lib', 'linker'):
                        embeds.append((state['cur'], pc.group(1), pragma_string(pc.group(2) or '', fail, m)))
                region(m.start() + line.index('#') + 1, m.end())   # #pragma, #ident: their own literals
                continue
            if g in ('str', 'raw'):
                flush_run()
                if not lit:
                    lit_file = state['cur']
                try:
                    lit.append(m.group('rbody') if g == 'raw' else decode_escapes(m.group('sbody')))
                except Unmodelled as e:
                    fail(m, str(e))
                continue
            flush_lit()
            if g == 'comma':
                if want_value:
                    flush_run()
                want_value = True
                continue
            v = None
            if g == 'chr':
                try:
                    c = decode_escapes(m.group('cbody'))
                except Unmodelled as e:
                    fail(m, str(e))
                v = ord(c) if len(c) == 1 else None
            elif g == 'num':
                v = int_value(m.group('num'))
            elif g == 'quote':
                fail(m, 'a lone quote (an unterminated literal)')
            elif g == 'id' and m.group(0) in RAW_IDS and text.startswith('"', m.end()):
                fail(m, 'a raw string clang\'s output does not close')
            if v is None:
                flush_run()
                continue
            if not want_value:
                flush_run()
            if not run:
                run_file = state['cur']
            run.append(v)
            want_value = False
        flush_lit()
        flush_run()

    region(0, len(text))
    return dict(names), entered, embeds


def pragma_string(arg, fail, m):
    """The string a `#pragma comment(lib | linker, <string>)` names: its literals' values concatenated; anything but
    string literals (a macro clang left unexpanded, a number) or no string at all is Unmodelled."""
    pieces = []
    for t in OUT_TOKEN.finditer(arg):
        g = t.lastgroup
        if g == 'ws':
            continue
        if g not in ('str', 'raw'):
            fail(m, 'a #pragma comment argument that is not a string literal')
        try:
            pieces.append(t.group('rbody') if g == 'raw' else decode_escapes(t.group('sbody')))
        except Unmodelled as e:
            fail(m, str(e))
    if not pieces:
        fail(m, 'a #pragma comment(lib | linker) with no string')
    return ''.join(pieces)


def output_names(text, directory, root):
    """({file: SR_* names}, files entered) of one -E output (output_scan without its embeds)."""
    names, entered, _ = output_scan(text, directory, root)
    return names, entered


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_receipts():
    """tools/pp_receipts.py, the project's receipt tool: its compile-database classification, command splitting and
    output stripping are reused, so both tools run a unit's command the same way."""
    return load_module('pp_receipts', os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                   'pp_receipts.py'))


def load_audit():
    """tools/player/composition_audit.py, the C1 composition audit: its build.ninja reader and link closure (to a
    fixpoint over every linker edge's inputs and link bindings) are reused, so both tools see the same exe."""
    return load_module('composition_audit', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                         'composition_audit.py'))


def canon(path):
    """A path compared across the build graph, the compile database and a command: absolute, normalised, case-folded
    as the file system folds it."""
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def textual_includes(args):
    """The command with every driver-level forced include (`-include H`, `--include H`, `--include=H`, `-includeH`)
    made cc1's (`-Xclang -include -Xclang H`): the driver swaps a precompiled H.pch / H.gch in for H when one exists,
    cc1's -include is always the text."""
    out, i = [], 0
    while i < len(args):
        a = args[i]
        if a == '-Xclang' and i + 3 < len(args) and args[i + 1] == '-include' and args[i + 2] == '-Xclang':
            out += args[i:i + 4]
            i += 4
            continue
        if a in ('-include', '--include') and i + 1 < len(args):
            h, i = args[i + 1], i + 2
        elif a.startswith('--include='):
            h, i = a.split('=', 1)[1], i + 1
        elif a.startswith('-include') and len(a) > len('-include') and not a.startswith('-include-pch'):
            h, i = a[len('-include'):], i + 1
        else:
            out.append(a)
            i += 1
            continue
        out += ['-Xclang', '-include', '-Xclang', h]
    return out


def cc1_includes(args):
    """The forced includes of a command after textual_includes(), in order."""
    return [args[i + 3] for i in range(len(args) - 3)
            if args[i] == '-Xclang' and args[i + 1] == '-include' and args[i + 2] == '-Xclang']


def resolve_pch(args, directory, db, pr):
    """(the command with each -include-pch replaced by the precompiled header's text, [(the producer's entry, its
    command with forced includes textual)]) (Astra round 7: a PCH dropped without its text could drop its contents).
    The PCH must be the output of a compile-database entry that emits it (-emit-pch); its forced includes and then
    its own source are included textually (`-Xclang -include -Xclang`) where the PCH was, so the unit reads the PCH's
    text and then its own includes, as clang does. That the text means what the PCH meant - the producer's macros,
    include paths and language - is checked separately (pch_check). A PCH no entry produces (a prebuilt .pch), a PCH
    built from another PCH, a forced include ahead of the PCH (clang reads the PCH first) or any -include-pch form left
    over is Unmodelled. A PCH edge's own `-Xclang -emit-pch` is dropped (-E replaces that frontend action)."""
    out, pchs, i = [], [], 0
    while i < len(args):
        a = args[i]
        nxt = args[i + 1] if i + 1 < len(args) else ''
        if a == '-Xclang' and nxt == '-include-pch' and i + 3 < len(args) and args[i + 2] == '-Xclang':
            pch, i = args[i + 3], i + 4
        elif a == '-Xclang' and nxt.startswith('-include-pch='):
            pch, i = nxt.split('=', 1)[1], i + 2
        elif a == '-include-pch' and i + 1 < len(args):
            pch, i = nxt, i + 2
        elif a.startswith('-include-pch='):
            pch, i = a.split('=', 1)[1], i + 1
        elif a == '-Xclang' and nxt == '-emit-pch':
            i += 2
            continue
        else:
            out.append(a)
            i += 1
            continue
        if cc1_includes(out):
            raise Unmodelled(f'-include-pch {pch}: a forced include comes before it (clang reads the PCH first)')
        e = db.get(canon(os.path.join(directory, pch)))
        if e is None:
            raise Unmodelled(f'-include-pch {pch}: no compile-database entry produces it, so its text cannot be read '
                             f'in its place')
        try:
            pargs = textual_includes(pr.command_args(e))
        except ValueError as ex:
            raise Unmodelled(f'-include-pch {pch}: its compile command cannot be split ({ex})') from None
        if '-emit-pch' not in pargs:
            raise Unmodelled(f'-include-pch {pch}: the entry that produces it ({e["file"]}) does not emit a PCH')
        if pr.pch_options(pargs):
            raise Unmodelled(f'-include-pch {pch}: built from another precompiled header')
        pchs.append((e, pargs))
        for h in cc1_includes(pargs) + [e['file']]:
            out += ['-Xclang', '-include', '-Xclang', os.path.join(e['directory'], h)]
    if pr.pch_options(out):
        raise Unmodelled(f'an -include-pch form this tool does not resolve: {pr.pch_options(out)}')
    return out, pchs


def without(args, directory, source, drop_includes=True):
    """A command without its source file, its outputs, its PCH options and (drop_includes) its forced includes."""
    out, i, src = [], 0, canon(os.path.join(directory, source))
    while i < len(args):
        a, nxt = args[i], args[i + 1] if i + 1 < len(args) else ''
        if a == '-Xclang' and nxt in ('-include', '-include-pch') and i + 3 < len(args) and args[i + 2] == '-Xclang':
            if nxt == '-include' and not drop_includes:
                out += args[i:i + 4]
            i += 4
        elif a == '-Xclang' and (nxt == '-emit-pch' or nxt.startswith('-include-pch=')):
            i += 2
        elif a == '-include-pch':
            i += 2
        elif a.startswith('-include-pch='):
            i += 1
        elif canon(os.path.join(directory, a)) == src:
            i += 1
        else:
            out.append(a)
            i += 1
    return out


def pch_check(e, base, pe, pargs, pr):
    """The check that a unit's PCH substitution reads what the PCH held: (key, the PCH's text under its producer's own
    command - its source the main file -, its directory, the substitution as the unit runs it - this unit's command,
    the PCH's forced includes and then its source force-included, the main file an empty stub -, its directory).
    Both are preprocessed with -E -dD (the text and every macro definition, the command line's included) and must
    print the same text once line markers and blank lines are set aside, or the unit is exit 2: a producer -D the unit
    lacks (Astra round 8's -DSWITCH=1, which a PCH carries into the unit), an include path that resolves a header
    elsewhere, or text that depends on its place (Astra round 9: __INCLUDE_LEVEL__ is 0 in the PCH's source and 1
    once it is force-included)."""
    pdir, cdir = pe['directory'], e['directory']
    src = os.path.join(pdir, pe['file'])
    inc = [x for h in cc1_includes(pargs) for x in ('-Xclang', '-include', '-Xclang', os.path.join(pdir, h))]
    prod = without([c for c in pr.strip_outputs(pargs) if c not in pr.PP_OUTPUT_FLAGS], pdir, pe['file']) + inc + [src]
    cons = without(base, cdir, e['file']) + inc + ['-Xclang', '-include', '-Xclang', src]
    return (canon(os.path.join(pdir, pe['output'])), tuple(prod), pdir, tuple(cons), cdir), prod, pdir, cons, cdir


def unit_args(e, x, db, pr):
    """(the -E command of one unit: its own, outputs stripped, forced includes textual, its PCH read as text; the
    checks of its PCH substitution)."""
    base = textual_includes([c for c in pr.strip_outputs(x) if c not in pr.PP_OUTPUT_FLAGS])
    args, pchs = resolve_pch(base, e['directory'], db, pr)
    return args, [pch_check(e, base, pe, pargs, pr) for pe, pargs in pchs]


def pch_differs(check, work, k):
    """None when the PCH's text reads the same under its producer's command and as the unit's substitution runs it,
    else where the two part. Both are preprocessed with -E -P -dD (no line markers: the producer's text is its main
    file, the substitution's an included one) and compared exactly; output that differs only in blanks is compared
    as tokens read by the -E reader (OUT_TOKEN: literals whole, a raw string's lines inside it - Astra round 10: a
    line-marker regex had deleted raw-string lines), never by a line pattern."""
    _, prod, pdir, cons, cdir = check
    stub = os.path.join(work, 'pch_stub.cpp')
    with open(stub, 'w', encoding='utf-8') as fh:
        fh.write('')
    texts = []
    for n, (cmd, d) in enumerate(((list(prod), pdir), (list(cons) + [stub], cdir))):
        out = os.path.join(work, f'pch_{k}_{n}.i')
        try:
            p = subprocess.run(cmd + ['-E', '-P', '-dD', '-o', out], cwd=d, capture_output=True, text=True,
                               errors='replace')
        except OSError as e:
            raise Unmodelled(f'the compiler cannot be run: {e}') from None
        try:
            if p.returncode:
                raise Unmodelled(f'the PCH text does not preprocess ({"producer" if n == 0 else "unit"} context, exit '
                                 f'{p.returncode}): {p.stderr.strip()[-300:]}')
            with open(out, encoding='utf-8', errors='replace') as fh:
                texts.append(fh.read().replace('\r\n', '\n'))
        finally:
            try:
                os.remove(out)
            except OSError:
                pass
    if texts[0] == texts[1]:
        return None
    a, b = ([m.group(0) for m in OUT_TOKEN.finditer(t) if m.lastgroup != 'ws'] for t in texts)
    if a == b:
        return None
    n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    return (f'token {n + 1}: the producer\'s {a[n] if n < len(a) else "<end>"!r}, this unit\'s '
            f'{b[n] if n < len(b) else "<end>"!r}')


def rc_detail(ca, path):
    """What a resource script holds, for the person judging it (composition_audit's .rc reader)."""
    try:
        with open(path, 'rb') as fh:
            text = ca.decode_source(fh.read())
    except OSError as e:
        return f'the resource script cannot be read ({e})'
    if text is None:
        return 'the resource script cannot be decoded'
    files, incs, probs = ca.rc_resources(text)
    parts = (['payload ' + ', '.join(files)] if files else []) + (['RCINCLUDE ' + ', '.join(incs)] if incs else [])
    return '; '.join(parts + probs) or 'no payload file (its statements are compiled into the exe)'


def target_of(obj):
    m = TARGET_DIR.search(obj.replace('\\', '/'))
    return m.group(1) if m else None


# ---------------------------------------------------------------------------------------------------------------------
# The link commands: a canonical-template allowlist (Astra round 11: reading a link command as ninja and a shell would
# read it kept leaking - ninja's own `compdb -x` swaps the response file in by an unbounded substring search, a
# response file's `&&` was read as a shell separator, a copy between two links passed as a post-link copy). Each linker
# edge of the exe (the exe's and each static library's it links) must have one of the command shapes the tree's four
# link edges use (LINK_TEMPLATES), exactly, and every value the shape reads comes from ninja itself:
#  - its rule's text (the graph with its includes inlined, link_rule) is a template's, the programs aside - each the
#    template's program (TEMPLATE_PROGRAMS) - and the rule binds nothing but LINK_RULE_KEYS: a response file as
#    `rspfile = $RSP_FILE` and `rspfile_content = $in (or $in_newline) $LINK_PATH $LINK_LIBRARIES`, exactly;
#  - ninja evaluates, for the edge, `$in`, `$in_newline`, `$rspfile`, `$rspfile_content` and each variable of the
#    template: `ninja -t compdb` on a scratch copy of the graph whose link rules' `command` is a record of them (ninja's
#    own scoping and binding order - an edge's own binding first - nothing expanded here), and the template filled with
#    those values must be the command ninja gives the edge in the real graph (`ninja -t compdb`, never -x);
#  - $PRE_LINK is `cd .`; $POST_BUILD is `cd .`, or for the executable (run once everything it links is built) CMake's
#    `cmd.exe /C "..."` of `cd /D <dir>` and `cmake -E copy* / make_directory` of absolute paths writing no file a
#    linker edge reads or writes; every other value on the command line holds no character cmd.exe reads
#    (SHELL_CHARS), so the template's `&&` are the only separators and its one linker or archiver the only link;
#  - the linker's one `@<file>` is the edge's rspfile exactly (quoted or not; any other @file is exit 2), and the
#    response file's content is split only as linker arguments (MSVC's rules, blanks and newlines separating) - no
#    shell operator (RSP_SHELL), no nested @file;
#  - every token the linker or archiver gets is an input the edge lists, one of its outputs, a token of the edge's own
#    link bindings (LINK_VARS - what the closure reads) or a flag of RULE_FLAGS; `cmake -E rm -f` and ranlib name the
#    edge's output only;
#  - (Astra round 12) every name the link reads is ASCII - a response file's leading BOM is one clang drops and the
#    closure keeps - and no step or link argument names a path through a Windows alias (path_alias: a namespace
#    prefix, UNC, an 8.3 name, an alternate data stream, a trailing dot or blank); a post-build copy's destination is
#    compared with the linker edges' files by resolved identity (file_identity, file_stat: a junction, a hard link).
# Anything else - another rule shape, a step, a second link, a token outside the allowlist, a graph ninja cannot answer
# for - is exit 2 until a template is added here; `ninja -t query` must still read each edge's inputs as the closure
# read them. exe_sweep on the built binary stays the backstop.
# ---------------------------------------------------------------------------------------------------------------------
META = '"&|<>^%!;'
PROG = r'(?:"[^"$\n&|<>^%!;]+"|[^\s"$&|<>^%!;]+)'          # a program path as a rule writes it, quoted or not
TEMPLATE_PROGRAMS = {'cmd': ('cmd',), 'cxx': ('clang++', 'clang'), 'cmake': ('cmake',), 'ar': ('llvm-ar',),
                     'ranlib': ('llvm-ranlib',)}
TEMPLATE_SLOTS = {'rsp': r'(?:@\$RSP_FILE|"@\$RSP_FILE"|@"\$RSP_FILE")', 'ver': r'\d+\.\d+'}
# CMake's clang-on-Windows link rules as the tree's link edges use them: {shape: (command, a response file, the
# executable's post-build copies allowed)}.
LINK_TEMPLATES = {
    'executable (clang++, a response file)': (
        '{cmd} /C "$PRE_LINK && {cxx} -nostartfiles -nostdlib $FLAGS $LINK_FLAGS {rsp} -o $TARGET_FILE -Xlinker '
        '/MANIFEST:EMBED -Xlinker /implib:$TARGET_IMPLIB -Xlinker /pdb:$TARGET_PDB -Xlinker /version:{ver}  '
        '$MANIFESTS && $POST_BUILD"', True, True),
    'static library (llvm-ar, $in)': (
        '{cmd} /C "$PRE_LINK && {cmake} -E rm -f $TARGET_FILE && {ar} qc $TARGET_FILE $LINK_FLAGS $in && {ranlib} '
        '$TARGET_FILE && $POST_BUILD"', False, False),
    'static library (llvm-ar, a response file)': (
        '{cmd} /C "$PRE_LINK && {cmake} -E rm -f $TARGET_FILE && {ar} qc $TARGET_FILE $LINK_FLAGS {rsp} && {ranlib} '
        '$TARGET_FILE && $POST_BUILD"', True, False),
}
RSP_CONTENTS = ('$in $LINK_PATH $LINK_LIBRARIES', '$in_newline $LINK_PATH $LINK_LIBRARIES')
LINK_RULE_KEYS = ('command', 'description', 'rspfile', 'rspfile_content', 'restat')
RECORD_VARS = ('in', 'in_newline', 'rspfile', 'rspfile_content', 'LINK_PATH', 'LINK_LIBRARIES')
RECORD = '<<gate_sweep:{}>>'
SHELL_CHARS = re.compile(r'["&|<>^%!;\r\n]')     # what cmd.exe reads on a command line (and `"`, which re-quotes)
RSP_SHELL = re.compile(r'[&|<>^%!;]')            # a shell operator in a response file: no linker argument holds one
POST_CD = re.compile(r'cd /D [^\s"&|<>^%!;]+')
POST_ITEM = re.compile(PROG + r'(?: [^\s"&|<>^%!;]+)+')
NON_ASCII = re.compile(r'[^\x00-\x7f]')          # a BOM or any non-ASCII byte: ninja, cmd.exe and clang read it apart
FLAG_PREFIX = re.compile(r'[/-][A-Za-z][A-Za-z0-9_-]*[:=]|-[LlI]|--[A-Za-z][A-Za-z0-9_-]*=')   # a flag before a value
# The flags a link template passes (CMake's clang-on-Windows link and archive rules): none takes an input.
RULE_FLAGS = re.compile(r'-nostartfiles|-nostdlib|-o|-Xlinker|qc|/MANIFEST:EMBED|[/-](?:implib|pdb):.+'
                        r'|[/-]version:\d+(?:\.\d+)?', re.I)
NINJA_INCLUDE = re.compile(r'^(include|subninja)[ \t]+(.+)$', re.M)


def template_re(t):
    out, i = [], 0
    for m in re.finditer(r'\{(\w+)\}', t):
        k = m.group(1)
        out += [re.escape(t[i:m.start()]), f'(?P<{k}>{PROG})' if k in TEMPLATE_PROGRAMS else TEMPLATE_SLOTS[k]]
        i = m.end()
    return re.compile(''.join(out + [re.escape(t[i:])]))


TEMPLATE_RES = {shape: (template_re(c), rsp, post) for shape, (c, rsp, post) in LINK_TEMPLATES.items()}


def excerpt(s, n=160):
    s = ' '.join(s.split())
    return s if len(s) <= 2 * n + 5 else s[:n] + ' ... ' + s[-n:]


def inline_includes(text, build, view, depth=0):
    """The graph's text with each `include` replaced by the included file's text (ninja reads an include in the
    including file's scope, as if it were written there). A subninja, an include path that is not a plain path, or an
    included file holding build edges (the closure's reader reads build.ninja's own text) is Unmodelled."""
    def inline(m):
        kw, name = m.group(1), m.group(2).strip()
        if kw == 'subninja':
            raise Unmodelled(f'the {view} build graph: {kw} {name} holds build edges this tool does not read')
        if '$' in name or depth > 8:
            raise Unmodelled(f'the {view} build graph: {kw} {name}: an include this tool does not read')
        try:
            with open(os.path.join(build, name), encoding='utf-8', errors='replace') as fh:
                inc = fh.read()
        except OSError as e:
            raise Unmodelled(f'the {view} build graph: {kw} {name}: {e}') from None
        if re.search(r'^build[ \t]', inc, re.M):
            raise Unmodelled(f'the {view} build graph: {kw} {name} holds build edges this tool does not read')
        return inline_includes(inc, build, view, depth + 1).rstrip('\r\n')
    return NINJA_INCLUDE.sub(inline, text)


def ninja_tool(build, ninja_file, args, view):
    """The output of `ninja -C <build> -f <file> -t <args>` - ninja's own reading of a graph; Unmodelled when ninja
    cannot be run or fails."""
    exe = shutil.which('ninja')
    if not exe:
        raise Unmodelled(f'the {view} build: ninja is not on PATH (the link commands are read from ninja itself)')
    p = subprocess.run([exe, '-C', build, '-f', os.path.abspath(ninja_file), '-t'] + list(args), capture_output=True,
                       text=True, errors='replace')
    if p.returncode:
        raise Unmodelled(f'the {view} build: `ninja -t {" ".join(args[:2])}` exited {p.returncode}: '
                         f'{(p.stderr or p.stdout).strip()[-300:]}')
    return p.stdout


def ninja_query(text):
    """{node: (rule, explicit, implicit, order-only)} of `ninja -t query` output."""
    out, cur, inputs = {}, None, False
    for line in text.splitlines():
        if not line.strip():
            continue
        if not line.startswith(' ') and line.endswith(':'):
            cur, inputs = line[:-1], False
            out[cur] = [None, [], [], []]
        elif line.startswith('  input: ') and cur is not None:
            out[cur][0], inputs = line[len('  input: '):].strip(), True
        elif line.startswith('    ') and inputs:
            v = line[4:]
            if v.startswith('|| '):
                out[cur][3].append(v[3:])
            elif v.startswith('| '):
                out[cur][2].append(v[2:])
            else:
                out[cur][1].append(v)
        else:
            inputs = False          # outputs:, validations:
    return out


def compdb_commands(text, build, view):
    """{output: [command]} of `ninja -t compdb` output."""
    try:
        entries = json.loads(text)
    except ValueError as e:
        raise Unmodelled(f'the {view} build: `ninja -t compdb` printed no JSON ({e})') from None
    out = {}
    for x in entries:
        out.setdefault(same_path(os.path.join(x.get('directory', build), x.get('output', '')), build), []).append(
            x.get('command', ''))
    return out


def win_args(s):
    """The arguments of a Windows command line or response file, as the MSVC runtime (and clang, lld-link, llvm-ar)
    reads them: blanks and newlines separate; `"` toggles quoting (`""` inside quotes is a quote); 2n backslashes
    before a quote are n backslashes and the quote toggles, 2n+1 are n and a literal quote; other backslashes are
    literal. ValueError on an unterminated quote."""
    args, cur, inq, have, i, n = [], [], False, False, 0, len(s)
    while i < n:
        c = s[i]
        if c == '\\':
            j = i
            while j < n and s[j] == '\\':
                j += 1
            if j < n and s[j] == '"':
                cur.append('\\' * ((j - i) // 2))
                if (j - i) % 2:
                    cur.append('"')
                    j += 1
            else:
                cur.append('\\' * (j - i))
            i, have = j, True
        elif c == '"':
            if inq and i + 1 < n and s[i + 1] == '"':
                cur.append('"')
                i += 2
            else:
                inq, i = not inq, i + 1
            have = True
        elif c in ' \t\r\n' and not inq:
            if have:
                args.append(''.join(cur))
            cur, have, i = [], False, i + 1
        else:
            cur.append(c)
            have, i = True, i + 1
    if inq:
        raise ValueError('an unterminated quote')
    if have:
        args.append(''.join(cur))
    return args


def program_name(prog, pr):
    """A program's name - its file's, an 8.3 alias resolved, no .exe - from its path (one argument)."""
    n = pr.driver_name(prog) if prog else ''
    return n[:-4] if n.endswith('.exe') else n


def unquoted(text):
    """A program path as a command spells it, quoted or not, as one argument."""
    try:
        return win_args(text)[0]
    except (ValueError, IndexError):
        return ''


def same_path(a, build):
    """The canonical path a command token names (relative to the build directory), or None if it is no path."""
    try:
        return canon(os.path.join(build, a))
    except (OSError, ValueError):
        return None


def link_rule(lines, name):
    """({key: (value, line index)}, None) of `rule <name>`'s bindings in the graph's text (its includes inlined), each
    one `key = value` line; (None, problem) when the graph does not define the rule once or the block holds anything
    else (a continued line, a comment)."""
    at = [k for k, line in enumerate(lines) if re.fullmatch(r'rule[ \t]+' + re.escape(name) + r'[ \t]*\r?', line)]
    if len(at) != 1:
        return None, f'the graph defines its rule {name} {len(at)} time(s)'
    binds, k = {}, at[0] + 1
    while k < len(lines) and lines[k][:1] in (' ', '\t'):
        m = re.fullmatch(r'[ \t]+([A-Za-z0-9_.-]+)[ \t]*=[ \t]*(.*?)\r?', lines[k])
        if not m or m.group(1) in binds or m.group(2).endswith('$'):
            return None, (f'its rule {name} holds a line this tool does not read as one binding '
                          f'({lines[k].strip()[:120]})')
        binds[m.group(1)] = (m.group(2), k)
        k += 1
    return binds, None


def link_shape(name, binds, pr):
    """(shape, match, a response file, post-build copies allowed, the rule's rspfile_content) of the LINK_TEMPLATES
    template a link rule's text is, or a problem."""
    extra = [k for k in binds if k not in LINK_RULE_KEYS]
    if extra:
        held = '; '.join(f'{k} = {binds[k][0]}' for k in extra)[:240]
        return f'its rule {name} binds {held} (a link template binds only {", ".join(LINK_RULE_KEYS)})'
    cmd = binds.get('command', ('', 0))[0]
    shown = '; '.join(f'{k} = {binds[k][0]}' for k in ('command', 'rspfile', 'rspfile_content') if k in binds)
    for shape, (rx, rsp, post) in TEMPLATE_RES.items():
        m = rx.fullmatch(cmd)
        if not m:
            continue
        rf, rc = (binds.get(k, (None,))[0] for k in ('rspfile', 'rspfile_content'))
        if not ((rf == '$RSP_FILE' and rc in RSP_CONTENTS) if rsp else (rf is None and rc is None)):
            return (f'its rule {name} ({shape}) writes a response file other than the template\'s '
                    f'(rspfile = {rf}; rspfile_content = {rc})')
        for k, names in TEMPLATE_PROGRAMS.items():
            got = program_name(unquoted(m.group(k)), pr) if k in rx.groupindex else names[0]
            if got not in names:
                return f'its rule {name} ({shape}) runs {got or m.group(k)} where the template runs {" / ".join(names)}'
        return shape, m, rsp, post, rc
    return (f'its rule {name} is none of the link command shapes this tool accepts (LINK_TEMPLATES): '
            f'{excerpt(shown, 200)}')


def template_vars(cmd):
    return list(dict.fromkeys(re.findall(r'\$([A-Za-z0-9_]+)', cmd)))


def fill(text, vals):
    """A template's text with each `$NAME` replaced by ninja's value of it for the edge."""
    return re.sub(r'\$([A-Za-z0-9_]+)', lambda m: vals[m.group(1)], text)


def read_record(cmd, names):
    """{name: value} of a scratch record (RECORD before each value, in order), or None when the command is no record
    (the edge binds its own `command`)."""
    parts = re.split(r'<<gate_sweep:([A-Za-z0-9_]+)>>', cmd)
    if len(parts) != 2 * len(names) + 3 or parts[0] or parts[-1] or parts[1::2] != list(names) + ['end']:
        return None
    return dict(zip(parts[1::2], parts[2::2]))


def path_alias(t):
    r"""Why a step or link argument may name a file through a Windows path alias (Astra round 12: a post-build copy
    to \\?\<the exe> overwrote it unseen) - a namespace prefix (\\?\, \\.\, \??\), a UNC path or a doubled separator,
    an 8.3 short name (a `~`), a colon past a drive letter (an alternate data stream, a drive-relative path), a path
    component ending in a dot or a blank - or None. No alias is resolved here: each is exit 2."""
    if re.search(r'[\\/][\\/]|[\\/]\?\?[\\/]', t):
        return 'a namespace prefix, a UNC path or a doubled separator'
    if '~' in t:
        return 'an 8.3 short name (~)'
    m = FLAG_PREFIX.match(t)
    v = t[m.end():] if m else t
    if ':' in (v[2:] if re.match(r'[A-Za-z]:[\\/]', v) else v):
        return 'a colon past a drive letter (an alternate data stream or a drive-relative path)'
    if any(c not in ('.', '..') and c[-1:] in ('.', ' ') for c in re.split(r'[\\/]', v)):
        return 'a path component ending in a dot or a blank'
    return None


def file_identity(p):
    """A path's resolved identity: its real path (junctions, symbolic links and 8.3 names resolved), case-folded."""
    return os.path.normcase(os.path.realpath(p))


def file_stat(p):
    """(volume, file index) of an existing file - the same for each of its hard links - or None."""
    try:
        st = os.stat(p)
    except (OSError, ValueError):
        return None
    return (st.st_dev, st.st_ino) if st.st_ino else None


def harmless_copy(toks, pr, protected):
    """A `cmake -E copy / copy_if_different / copy_directory[_if_different] / make_directory` of absolute paths that
    cannot write a file a linker edge reads or writes, compared by resolved identity (protected: their file_identity
    set and their file_stat set - a hard link): its destination is none of them, a directory receives no file that
    is one of them, a copied directory's destination holds none of them."""
    if len(toks) < 4 or program_name(toks[0], pr) != 'cmake' or toks[1] != '-E' or not all(
            os.path.isabs(a) for a in toks[3:]):
        return False
    op, args = toks[2], toks[3:]
    if op == 'make_directory':
        return True
    ids, stats = protected
    dst = file_identity(args[-1])
    if op in ('copy', 'copy_if_different') and len(args) >= 2:
        written = [dst] + [file_identity(os.path.join(dst, os.path.basename(a))) for a in args[:-1]]
        return not any(w in ids or file_stat(w) in stats for w in written)
    if op in ('copy_directory', 'copy_directory_if_different') and len(args) >= 2:
        return not any(p == dst or p.startswith(dst.rstrip(os.sep) + os.sep) for p in ids)
    return False


def path_problem(node, where, t):
    why = path_alias(t)
    return f'{node}: {where} names {t!r} - {why}: a path this tool does not resolve' if why else None


def post_build_problems(node, value, pr, protected):
    """The executable's post-build step: CMake's `cmd.exe /C "<item> && ..."`, each item `cd /D <dir>` or a
    harmless_copy, no argument a path alias."""
    m = re.fullmatch('(' + PROG + r') /C "(.*)"', value, re.S)
    if not m or program_name(unquoted(m.group(1)), pr) != 'cmd':
        return [f'{node}: a post-build step ({excerpt(value)}) - only `cd .` or CMake\'s cmd.exe /C "..." of copies '
                f'runs after the link']
    probs = []
    for item in m.group(2).split(' && '):
        toks = win_args(item) if POST_CD.fullmatch(item) or POST_ITEM.fullmatch(item) else None
        bad = [p for p in (path_problem(node, 'a post-build step', a) for a in (toks or [])[1:]) if p]
        if bad:
            probs += bad[:1]
        elif not (toks and (POST_CD.fullmatch(item) or harmless_copy(toks, pr, protected))):
            probs.append(f'{node}: a post-build step this tool does not accept ({excerpt(item)}) - only `cd /D` and a '
                         f'cmake -E copy* / make_directory of absolute paths writing no file a linker edge reads or '
                         f'writes')
    return probs


def link_edge_problems(L, real, vals, shape, ca, pr, build, protected):
    """The problems of one linker edge whose rule is a template (see above): real is the command ninja gives it, vals
    ninja's values of the template's variables for it."""
    node, edge = L['node'], L['edge']
    sname, m, has_rsp, post_ok, content, cmd = shape
    # Astra round 12: ninja writes a response file's bytes as the graph holds them, and clang drops a leading UTF-8
    # BOM (the closure reads the name with it); cmd.exe reads a command in the ANSI code page. ASCII only, no decoding.
    texts = [real] + ([vals['rspfile'], vals['rspfile_content']] if vals else []) + [
        p for part in ('explicit', 'implicit', 'outputs') for p in edge[part]]
    odd = next((x for x in texts if NON_ASCII.search(x)), None)
    if odd is not None:
        c = NON_ASCII.search(odd).start()
        return [f'{node}: its link command, response file or a name it links holds a non-ASCII character '
                f'({ascii(odd[max(0, c - 40):c + 40])}) - a BOM or a non-ASCII byte reads differently in ninja, '
                f'cmd.exe and the linker']
    if vals is None:
        return [f'{node}: ninja does not run its rule\'s command for it (an edge binding of `command`?): '
                f'{excerpt(real)}']
    if fill(cmd, vals) != real:
        return [f'{node}: the command ninja gives it is not its rule\'s template ({sname}) filled with ninja\'s '
                f'values: {excerpt(real)}']
    probs = []
    if vals['PRE_LINK'] != 'cd .':
        probs.append(f'{node}: a pre-link step ({excerpt(vals["PRE_LINK"])}) - only CMake\'s `cd .` runs before the '
                     f'link')
    if vals['POST_BUILD'] != 'cd .':
        probs += (post_build_problems(node, vals['POST_BUILD'], pr, protected) if post_ok else
                  [f'{node}: a post-build step ({excerpt(vals["POST_BUILD"])}) - only CMake\'s `cd .` runs after a '
                   f'library\'s archive'])
    for v in template_vars(cmd):
        if v not in ('PRE_LINK', 'POST_BUILD') and SHELL_CHARS.search(vals[v]):
            probs.append(f'{node}: its ${v} ({excerpt(vals[v], 80)}) holds a character cmd.exe reads ({META}, a line '
                         f'break)')
    if has_rsp and (vals['rspfile'] != vals['RSP_FILE'] or vals['rspfile_content'] != fill(content, vals)):
        probs.append(f'{node}: ninja writes it a response file other than its rule\'s (an edge binding?): '
                     f'{vals["rspfile"][:80]}: {excerpt(vals["rspfile_content"])}')
    elif not has_rsp and (vals['rspfile'] or vals['rspfile_content']):
        probs.append(f'{node}: ninja writes it a response file its template does not read ({vals["rspfile"][:80]})')
    if probs:
        return probs
    mine = set()                      # the edge's own link bindings: what composition_audit's closure reads
    for v in ca.LINK_VARS:
        if v in edge['vars']:
            mine.update(ca.tokens(edge['vars'][v]))
    inputs = {same_path(p, build) for p in edge['explicit']}
    outputs = {same_path(p, build) for p in edge['outputs']}

    def allowed(t):
        return same_path(t, build) in inputs | outputs or t in mine or RULE_FLAGS.fullmatch(t)

    inner = cmd[m.end('cmd') + len(' /C "'):-1].split(' && ')     # the template's $PRE_LINK && ... && $POST_BUILD
    for seg in inner[1:-1]:
        argv = win_args(fill(seg, vals))
        name = program_name(argv[0], pr)
        bad = [p for p in (path_problem(node, 'its link command', t[1:] if t.startswith('@') else t)
                           for t in argv[1:]) if p]
        if bad:
            probs += bad
            continue
        if name == 'cmake':           # -E rm -f $TARGET_FILE
            if len(argv) != 5 or same_path(argv[4], build) not in outputs:
                probs.append(f'{node}: its `cmake -E rm -f` removes {argv[4:]} - not its output')
        elif name == 'llvm-ranlib':
            if len(argv) != 2 or same_path(argv[1], build) not in outputs:
                probs.append(f'{node}: its ranlib indexes {argv[1:]} - not its output')
        else:                         # the template's one linker / archiver
            seen = False
            for t in argv[1:]:
                if t.startswith('@'):
                    if has_rsp and not seen and t[1:] == vals['rspfile']:
                        seen = True
                    else:
                        probs.append(f'{node}: its link command reads a response file other than the edge\'s own '
                                     f'({vals["rspfile"] if has_rsp else "none"}): {t}')
                elif not allowed(t):
                    probs.append(f'{node}: its link command passes {t!r} - not an input the edge lists, one of its '
                                 f'outputs, a token of its own link bindings (what the closure reads) or a flag this '
                                 f'tool knows')
            if has_rsp and not seen:
                probs.append(f'{node}: its link command does not read its response file')
    if has_rsp:
        text = vals['rspfile_content']
        bad = RSP_SHELL.search(text)
        if bad:
            return probs + [f'{node}: its response file holds {text[bad.start()]!r} '
                            f'({excerpt(text[max(0, bad.start() - 60):bad.start() + 60], 60)}) - a response file is '
                            f'linker arguments only, and no argument the closure reads holds a shell operator']
        try:
            toks = win_args(text)
        except ValueError as e:
            return probs + [f'{node}: its response file cannot be split as the linker splits it ({e})']
        for t in toks:
            alias = path_problem(node, 'its response file', t)
            if alias:
                probs.append(alias)
            elif t.startswith('@'):
                probs.append(f'{node}: its response file names another response file ({t})')
            elif not allowed(t):
                probs.append(f'{node}: its response file passes {t!r} - not an input the edge lists, one of its '
                             f'outputs, a token of its own link bindings (what the closure reads) or a flag this tool '
                             f'knows')
    return probs


def link_commands(view, ninja_file, full, build, mine, ca, pr, work):
    """The problems of the exe's linker edges (see above): each edge as ninja reads it (`-t query`) must be the one the
    closure read, its rule one of LINK_TEMPLATES, and its command - the template filled with ninja's values, as
    `ninja -t compdb` gives it - must pass link_edge_problems."""
    probs = []
    nodes = [L['node'] for L in mine]
    query = {same_path(k, build): v for k, v in ninja_query(ninja_tool(build, ninja_file, ['query'] + nodes,
                                                                        view)).items()}
    for L in mine:
        q = query.get(same_path(L['node'], build))
        e = L['edge']
        got = (e['rule'],) + tuple({same_path(p, build) for p in e[part]} for part in ('explicit', 'implicit',
                                                                                       'order_only'))
        if q is None or (q[0],) + tuple({same_path(p, build) for p in part} for part in q[1:]) != got:
            probs.append(f'{L["node"]}: ninja reads its edge (rule, inputs) differently from the closure\'s reader')
    rules = sorted({L['edge']['rule'] for L in mine})
    real = compdb_commands(ninja_tool(build, ninja_file, ['compdb'] + rules, view), build, view)
    lines = full.split('\n')
    shapes, names = {}, {}
    for r in rules:
        binds, why = link_rule(lines, r)
        shape = why or link_shape(r, binds, pr)
        if isinstance(shape, str):
            shapes[r] = shape
            continue
        cmd, k = binds['command']
        names[r] = list(dict.fromkeys(template_vars(cmd) + list(RECORD_VARS)))
        lines[k] = (re.match(r'[ \t]+', lines[k]).group(0) + 'command = '
                    + ''.join(RECORD.format(v) + '${' + v + '}' for v in names[r]) + RECORD.format('end'))
        shapes[r] = shape + (cmd,)
    recorded = {}
    if names:                         # ninja's values for each edge: the graph with its link rules' command a record
        os.makedirs(work, exist_ok=True)
        scratch = os.path.join(work, f'link_values_{view}.ninja')
        with open(scratch, 'w', encoding='utf-8', newline='') as fh:
            fh.write('\n'.join(lines))
        recorded = compdb_commands(ninja_tool(build, scratch, ['compdb'] + sorted(names), view), build, view)
    files = {same_path(p, build) for L in mine for part in ('outputs', 'explicit', 'implicit') for p in L['edge'][part]}
    files.discard(None)
    protected = ({file_identity(p) for p in files}, {s for s in map(file_stat, files) if s})
    for L in mine:
        r, key = L['edge']['rule'], same_path(L['node'], build)
        if isinstance(shapes[r], str):
            probs.append(f'{L["node"]}: {shapes[r]}')
            continue
        got, rec = real.get(key, []), recorded.get(key, [])
        if len(got) != 1 or len(rec) != 1:
            probs.append(f'{L["node"]}: ninja gives {len(got)} command(s) for its edge')
            continue
        probs += link_edge_problems(L, got[0], read_record(rec[0], names[r]), shapes[r], ca, pr, build, protected)
    return probs


def add_external(uncovered, name, note):
    """Add a library the exe takes in to {item: detail} (one item per name, case-insensitively, as the linker does)."""
    name = name if os.path.splitext(name)[1] else name + '.lib'
    key = next((k for k in uncovered if k.lower() == f'external {name}'.lower()), f'external {name}')
    old = uncovered.get(key)
    uncovered[key] = note if not old else old if note in old else f'{old}; {note}'
    return key


def release_units(view, ninja_file, compdb, pr, ca, root, exe=RELEASE_EXE, work=None):
    """One build's release units, from its own build graph: {'units': [(key, 'rel', src, entry, args, build)],
    'uncovered': {item: detail}, 'objects': every closure object (DLLs' too), 'components': {component: units},
    'db': the compile database by output, 'built_libs': {library basename: linked by the exe}, 'pch_checks': [...]}.
    The units are the C / C++ compile units of exe's link closure (composition_audit's Graph.closure: the exe's own
    objects and every static library it links, sr_imgui and xenos_xlat included); each must have its compile-database
    entry, for the same source, runnable as pp_receipts runs a unit, its PCH resolved. UNCOVERED: a resource script or
    any other non-C / C++ unit, a DLL (its units), a delay-loaded DLL, a link input the graph does not build, a
    library a unit's --dependent-lib asks for. Unmodelled (every problem listed) when the graph has no edge for exe, a
    link input cannot be read, ninja reads a linker edge differently, or its rule is none of LINK_TEMPLATES or its
    command (the template filled with ninja's values) runs a step or may add an input (link_commands), a linked unit
    fails one of the above, or a REQUIRED_TARGETS target has no unit."""
    try:
        with open(ninja_file, encoding='utf-8', errors='replace') as fh:
            text = fh.read()
    except OSError as e:
        raise Unmodelled(f'the {view} build graph: {e}') from None
    edges, _ = ca.parse_ninja(text)
    build = os.path.dirname(os.path.abspath(ninja_file))
    full = inline_includes(text, build, view)
    g = ca.Graph(edges, build.replace('\\', '/'), root.replace('\\', '/'))
    try:
        units, dlls, libs, externals, linkers, link_fails, delays, _steps = g.closure(exe)
    except ValueError as e:
        raise Unmodelled(f'the {view} build graph {ninja_file}: {e}') from None
    comp = os.path.basename(exe)
    db = {}
    for e in compdb:
        if e.get('output') and e.get('directory'):
            db.setdefault(canon(os.path.join(e['directory'], e['output'])), e)
    sel, uncovered, problems, objects, comps, checks = [], {}, [], set(), collections.Counter(), []
    dependent = collections.defaultdict(set)
    for u in units:
        obj = canon(os.path.join(build, u['object']))
        objects.add(obj)
        if u['component'] != comp:
            continue                                  # a DLL's unit: the DLL is UNCOVERED as a whole
        src_abs = os.path.join(build, u['src_node'])
        src = unit_path(src_abs, root, build)
        rule = edges[u['object']]['rule']
        if not rule.startswith(C_RULES):
            if rule.startswith('RC_COMPILER__'):
                uncovered[f'resource {src}'] = rc_detail(ca, src_abs)
            else:
                uncovered[f'unit {src}'] = f'compiled by {rule.split("__")[0]}'
            continue
        e = db.get(obj)
        if e is None:
            problems.append(f'{src} ({u["object"]}): linked into {comp} by the build graph, but the compile database '
                            f'has no entry for it')
            continue
        if canon(os.path.join(e['directory'], e['file'])) != canon(src_abs):
            problems.append(f'{u["object"]}: the build graph compiles {src}, the compile database {e["file"]}')
            continue
        what, x = pr.classify_entry(e)
        if what != 'unit':
            problems.append(f'{src} ({u["object"]}): not a compile edge gate_sweep can run ({x or what})')
            continue
        try:
            args, pc = unit_args(e, x, db, pr)
        except Unmodelled as ex:
            problems.append(f'{src} ({u["object"]}): {ex}')
            continue
        for a in args:   # -Xclang --dependent-lib=X: the object asks the linker for X (/DEFAULTLIB), as a pragma does
            if a.startswith('--dependent-lib='):
                dependent[a.split('=', 1)[1]].add(src)
        comps[os.path.basename(u['lib']) if u['lib'] else comp] += 1
        sel.append((u['object'], 'rel', src, e, args, build))
        checks += [(src,) + c for c in pc]
    dll_notes = collections.defaultdict(list)
    for name, d in dlls.items():
        dll_notes[name].append(f'{d["units"]} compiled unit(s) linked into it by this graph')
    for d in delays:
        if d['component'] == comp:
            dll_notes[d['dll']].append(f'delay-loaded by {comp}')
    for name, notes in dll_notes.items():
        uncovered[f'dll {name}'] = '; '.join(notes)
    hows = collections.defaultdict(set)
    for x in externals:
        if x['component'] != comp:
            continue                                  # inside a DLL: the DLL's item covers it
        node = x['node']
        name = g.rel(node) if re.search(r'[\\/]', node) else node
        key = f'{"generated" if x["generated"] else "external"} {name}'
        hows[key].add(x.get('how') or 'link input')
    for key, h in hows.items():
        uncovered[key] = ('produced by a custom command' if key.startswith('generated') else
                          'a link input this graph does not build') + f' ({", ".join(sorted(h))})'
    for name, srcs in sorted(dependent.items()):
        add_external(uncovered, name, f'--dependent-lib of {len(srcs)} unit(s)')
    mine = [L for L in linkers if L['component'] == comp]
    problems += link_commands(view, ninja_file, full, build, mine, ca, pr,
                              work or os.path.join(root, 'archive', 'gate_sweep_tmp'))
    for f in link_fails:
        if any(L['node'] == f['edge'] for L in mine):
            problems.append(f'{f["edge"]}: the link input {f["input"]!r} cannot be read ({f["why"]})')
    for t in REQUIRED_TARGETS:
        if not any(target_of(u[0]) == t for u in sel):
            problems.append(f'no unit of the required target {t} in the link closure of {comp}')
    if problems:
        raise Unmodelled(f'the {view} build ({ninja_file}): {len(problems)} problem(s): ' + '; '.join(problems[:12])
                         + (' ...' if len(problems) > 12 else ''))
    linked = ({os.path.basename(k).lower() for k in libs}
              | {os.path.basename(i).lower() for d in dlls.values() for i in d['import_libs']})
    built = {os.path.basename(o).lower(): os.path.basename(o).lower() in linked for o, ed in edges.items()
             if ed['rule'].startswith(ca.STATIC_RULE_PREFIXES + ca.SHARED_RULE_PREFIXES) and o.lower().endswith('.lib')}
    return {'units': sel, 'uncovered': uncovered, 'objects': objects, 'components': dict(comps), 'db': db,
            'built_libs': built, 'pch_checks': checks, 'build': build}


def not_rel_units(compdb, objects, db, pr, root, build):
    """(the default database's units outside the exe's closure whose source is under ROOTS - kind 'nrel': difftest's
    and the tools' units, their literals count only for files under NOT_REL_DIRS, but every file they enter is
    reachable -, their PCH checks). A unit that cannot be run as a GNU-style compile (pp_receipts' scope) is exit 2."""
    out, checks = [], []
    for e in compdb:
        if e.get('output') and canon(os.path.join(e['directory'], e['output'])) in objects:
            continue
        src = unit_path(os.path.join(e['directory'], e['file']), root, build)
        if not src.startswith(tuple(r + '/' for r in ROOTS)):
            continue
        what, x = pr.classify_entry(e)
        if what == 'other':      # not a C / C++ translation unit: nothing to preprocess
            continue
        if what != 'unit':
            raise Unmodelled(f'{src} ({e.get("output")}): not a compile edge gate_sweep can run ({x or what})')
        args, pc = unit_args(e, x, db, pr)
        out.append((e.get('output') or e['file'], 'nrel', src, e, args, build))
        checks += [(src,) + c for c in pc]
    return out, checks


def shows_embeds(driver, work):
    """Does `driver -E -Xclang -dE` print an #embed directive instead of its bytes? (pp_receipts' probe, run in this
    run's own work folder.) Without it an embed could not be told from an initializer."""
    d = os.path.join(work, 'embed_probe')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'blob.bin'), 'wb') as fh:
        fh.write(b'SR_')
    with open(os.path.join(d, 'probe.cpp'), 'w', encoding='utf-8') as fh:
        fh.write('const unsigned char b[] = {\n#embed "blob.bin"\n};\n')
    try:
        p = subprocess.run([driver, '-E', '-Xclang', '-dE', 'probe.cpp'], cwd=d, capture_output=True, text=True,
                           errors='replace')
    except OSError as e:
        raise Unmodelled(f'the compiler cannot be run: {driver}: {e}') from None
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    return (p.returncode == 0 and any(EMBED_DIR.match(l) for l in lines)
            and not any(re.search(r'\b83\b', l) for l in lines if not l.lstrip().startswith('#')))


def preprocess_names(job):
    """One unit, one view: (key, ok, error, {file: names}, entered, embeds). Runs in a worker process."""
    key, args, directory, out_path, root, build = job
    try:
        p = subprocess.run(args + ['-E', '-Xclang', '-dE', '-o', out_path], cwd=directory, capture_output=True,
                           text=True, errors='replace')
    except OSError as e:
        return key, False, f'the compiler cannot be run: {e}', {}, set(), []
    try:
        if p.returncode:
            return key, False, f'the preprocessor exited {p.returncode}: {p.stderr.strip()[-400:]}', {}, set(), []
        with open(out_path, encoding='utf-8', errors='replace') as fh:
            names, entered, embeds = output_scan(fh.read(), directory, root, build)
        return key, True, None, names, entered, embeds
    except Unmodelled as e:
        return key, False, f'its -E output cannot be read: {e}', {}, set(), []
    except OSError as e:
        return key, False, f'no -E output: {e}', {}, set(), []
    finally:
        try:
            os.remove(out_path)
        except OSError:
            pass


def compdb_of(build_dir, compdb_file):
    """The compile database: `ninja -C <build_dir> -t compdb`, or a compile_commands.json given directly."""
    if compdb_file:
        with open(compdb_file, encoding='utf-8') as fh:
            return json.load(fh)
    p = subprocess.run(['ninja', '-C', build_dir, '-t', 'compdb'], capture_output=True, text=True, errors='replace')
    if p.returncode:
        raise Unmodelled(f'`ninja -C {build_dir} -t compdb` exited {p.returncode}: {p.stderr.strip()[-300:]}')
    return json.loads(p.stdout)


def clang_sweep(root, build, build_rel, compdb=None, compdb_rel=None, jobs=6, work=None, ninja=None, ninja_rel=None,
                exe=RELEASE_EXE):
    """The clang views: (removed {name: [files]}, surviving {name: [[file, where]]}, files entered, stats, uncovered
    {item: {'views', 'detail'}}). Raises Unmodelled when a database or graph cannot be read, a linked unit has no
    entry or cannot be run, a link rule may add an input, a required target is missing, the compiler cannot show
    embeds, a PCH's text reads differently in a unit than in its producer, a unit fails to preprocess or its output
    cannot be read, or a #pragma comment(lib) names a library the graph builds and the exe does not link."""
    pr, ca = load_receipts(), load_audit()
    root = os.path.abspath(root)
    work = os.path.abspath(work or os.path.join(root, 'archive', 'gate_sweep_tmp'))   # clang runs in each unit's dir
    os.makedirs(work, exist_ok=True)
    picked, uncovered = {}, {}
    for view, b, c, n in (('default', build, compdb, ninja), ('release', build_rel, compdb_rel, ninja_rel)):
        n = n or (os.path.join(b, 'build.ninja') if b else None)
        if not n:
            raise Unmodelled(f'the {view} build graph: --ninja{"-rel" if view == "release" else ""} (or the build dir)')
        db_list = compdb_of(b, c)
        picked[view] = release_units(view, n, db_list, pr, ca, root, exe, work)
        picked[view]['list'] = db_list
        for k, d in picked[view]['uncovered'].items():
            uncovered.setdefault(k, {'views': [], 'detail': d})['views'].append(view)
    dv, rv = picked['default'], picked['release']
    nrel, nrel_checks = not_rel_units(dv['list'], dv['objects'], dv['db'], pr, root, dv['build'])
    for driver in sorted({u[4][0] for u in dv['units'] + rv['units'] + nrel}):
        if not shows_embeds(driver, work):
            raise Unmodelled(f'{driver} does not print #embed under -E -Xclang -dE: an embed could not be told from '
                             f'its bytes')
    # Each PCH substitution must read what the PCH held: its text under its producer's command and under the unit's.
    seen, failures = {}, []
    for k, (src, key, prod, pdir, cons, cdir) in enumerate(dv['pch_checks'] + rv['pch_checks'] + nrel_checks):
        if key not in seen:
            seen[key] = pch_differs((key, prod, pdir, cons, cdir), work, k)
        if seen[key]:
            failures.append(f'{src}: its PCH was built in another preprocessing context - the PCH text under the '
                            f'producer\'s command and under this unit\'s differ at {seen[key]}')
    if failures:
        raise Unmodelled(f'{len(failures)} unit(s) cannot read their PCH as text: ' + '; '.join(failures[:6])
                         + (' ...' if len(failures) > 6 else ''))
    jobs_list = []
    for view, units in (('1', dv['units'] + nrel), ('0', rv['units'])):
        for k, (key, kind, src, e, args, b) in enumerate(units):
            jobs_list.append(((view, kind, src, key), args, e['directory'], os.path.join(work, f'v{view}_{k}.i'),
                              root, b))
    D, R, N = (collections.defaultdict(set) for _ in range(3))
    entered = set()
    asked = collections.defaultdict(lambda: collections.defaultdict(set))   # {item: {view: files}} of #pragma comment
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
        for key, ok, err, names, ent, inputs in ex.map(preprocess_names, jobs_list, chunksize=2):
            view, kind, src, _ = key
            if not ok:
                failures.append(f'{src} (SR_ORACLE={view} view): {err}')
                continue
            entered |= ent
            target = R if view == '0' else (D if kind == 'rel' else N)
            for f, ns in names.items():
                target[f] |= ns
            if kind != 'rel':
                continue
            v = 'release' if view == '0' else 'default'
            for f, what, value in inputs:
                if what in ('embed', 'incbin'):
                    item = uncovered.setdefault(f'embed {f} {value}', {'views': [], 'detail': f'in {src}'})
                    if v not in item['views']:
                        item['views'].append(v)
                elif what == 'linker':
                    asked[f'linker {value}'][v].add(f)
                else:                                     # 'lib': reconciled with the libraries the graph builds
                    name = value if os.path.splitext(value)[1] else value + '.lib'
                    linked = picked[v]['built_libs'].get(os.path.basename(name).lower())
                    if linked is False:
                        failures.append(f'{f}: #pragma comment(lib, "{value}") asks for {name}, which the {v} graph '
                                        f'builds and the exe does not link - its units are in neither view')
                    elif linked is None:
                        asked[name][v].add(f)
    if failures:
        raise Unmodelled(f'{len(failures)} unit(s) failed to preprocess: ' + '; '.join(failures[:8])
                         + (' ...' if len(failures) > 8 else ''))
    for name, by_view in asked.items():
        files = sorted(set().union(*by_view.values()))
        note = (f'asked for by #pragma comment({"linker" if name.startswith("linker ") else "lib"}) in '
                + ', '.join(files[:4]) + (f' and {len(files) - 4} more file(s)' if len(files) > 4 else ''))
        if name.startswith('linker '):
            key = name
            uncovered.setdefault(key, {'views': [], 'detail': note})
        else:
            flat = {k: u['detail'] for k, u in uncovered.items()}
            key = add_external(flat, name, note)
            uncovered.setdefault(key, {'views': [], 'detail': ''})['detail'] = flat[key]
        for v in ('default', 'release'):
            if v in by_view and v not in uncovered[key]['views']:
                uncovered[key]['views'].append(v)
    removed = collections.defaultdict(list)
    for f in sorted(D):
        for n in sorted(D[f] - R.get(f, set())):
            removed[n].append(f)
    surviving = {}
    for n in sorted(removed):
        keep = [[g, 'REL'] for g in sorted(R) if n in R[g]]
        keep += [[g, 'NOT-REL'] for g in sorted(N) if n in N[g] and g.startswith(NOT_REL_DIRS)]
        if keep:
            surviving[n] = keep
    dsrc, rsrc = {u[2] for u in dv['units']}, {u[2] for u in rv['units']}
    stats = {'exe': exe, 'units_default': len(dv['units']), 'units_release': len(rv['units']),
             'units_not_rel': len(nrel), 'files_entered': len(entered),
             'files_entered_build_dir': sum(1 for f in entered if f.startswith('<build>/')),
             'pch_contexts_checked': len(seen),
             'components': {'default': dv['components'], 'release': rv['components']},
             'dropped_by_release': sorted(dsrc - rsrc), 'added_by_release': sorted(rsrc - dsrc)}
    return dict(removed), surviving, entered, stats, uncovered


def read_acks(path):
    """--uncovered-ok: one UNCOVERED item per line, exactly as the report prints it; `#` lines and blank lines skipped
    (put the reason on a comment line above the item)."""
    with open(path, encoding='utf-8') as fh:
        return {s for s in (l.strip() for l in fh) if s and not s.startswith('#')}


def scan_files(root):
    """Every .cpp .hpp .h .inc .inl under ROOTS (not the generated guest/ppc), repo-relative, sorted."""
    files = []
    for r in ROOTS:
        for p in glob.glob(os.path.join(root, r, '**', '*'), recursive=True):
            q = os.path.relpath(p, root).replace(os.sep, '/')
            if q.endswith(EXT) and not q.startswith('guest/ppc/') and os.path.isfile(p):
                files.append(q)
    return sorted(files)


def report(removed, surviving):
    lines = []
    for n, keep in surviving.items():
        lines.append(f"{n}\tremoved from {','.join(removed[n])}\tSURVIVES {', '.join(f'{f} [{w}]' for f, w in keep)}")
    lines.append(f'surviving names: {len(surviving)}')
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--build', help='the default build dir (SR_ORACLE=1): its build.ninja and `ninja -t compdb`')
    ap.add_argument('--build-rel', help='the release build dir (SR_RELEASE_COMPOSITION=ON): its build.ninja and '
                                        '`ninja -t compdb`')
    ap.add_argument('--compdb', help='a compile_commands.json for the default view, in place of `ninja -t compdb`')
    ap.add_argument('--compdb-rel', help='a compile_commands.json for the release view')
    ap.add_argument('--ninja', help='the default build graph, in place of <--build>/build.ninja')
    ap.add_argument('--ninja-rel', help='the release build graph, in place of <--build-rel>/build.ninja')
    ap.add_argument('--exe', default=RELEASE_EXE, help='the exe whose link closure is the release (%(default)s)')
    ap.add_argument('--uncovered-ok', help='a file listing the UNCOVERED items a person has judged (one per line, as '
                                           'printed; `#` lines are comments): any other is exit 2')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--json', help='write the removed and surviving names here (exe_sweep.py --removed)')
    ap.add_argument('--advisory', action='store_true', help='the lexer only: fast, advisory, never the authority')
    ap.add_argument('--view', nargs=2, metavar=('FILE', 'V'), help='print FILE as the lexer reads SR_ORACLE=V (advisory)')
    ap.add_argument('--calls', help='(advisory) write the lexer\'s #if function-like allowlist and every use of it here')
    a = ap.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):  # a console; not when a caller captures stdout
        sys.stdout.reconfigure(encoding='utf-8')
    if hasattr(sys.stderr, 'reconfigure'):  # a problem may quote a non-ASCII name: printed, never a crash (exit 2)
        sys.stderr.reconfigure(errors='backslashreplace')
    try:
        if a.view:
            if a.view[1] not in ('0', '1'):
                raise Unmodelled('--view takes 0 or 1')
            sys.stdout.write(view(read(a.view[0]), int(a.view[1]), a.view[0].replace(os.sep, '/')))
            return 0
        if a.advisory:
            files, oracle, removed, surviving, summaries = sweep(a.root)
            print(f'advisory (lexer, not the authority): files {len(files)}, ORACLE_SOURCES {len(oracle)}, '
                  f'removed names {len(removed)}')
            print('\n'.join(report(removed, surviving)))
            if a.json:
                with open(a.json, 'w', encoding='utf-8') as fh:
                    json.dump({'mode': 'advisory-lexer', 'removed': removed, 'surviving': surviving}, fh, indent=1)
            if a.calls:
                with open(a.calls, 'w', encoding='utf-8') as fh:
                    fh.write('\n'.join(call_report(summaries)) + '\n')
            return 0
        if not (a.build or (a.compdb and a.ninja)) or not (a.build_rel or (a.compdb_rel and a.ninja_rel)):
            raise Unmodelled('the clang views need the default and the release build (--build / --build-rel, or '
                             '--compdb + --ninja / --compdb-rel + --ninja-rel); --advisory runs the lexer alone')
        acks = read_acks(a.uncovered_ok) if a.uncovered_ok else set()
        removed, surviving, entered, stats, uncovered = clang_sweep(
            a.root, a.build, a.build_rel, a.compdb, a.compdb_rel, a.jobs, ninja=a.ninja, ninja_rel=a.ninja_rel,
            exe=a.exe)
    except (OSError, ValueError, Unmodelled) as e:
        print(f'gate_sweep: cannot run: {e}', file=sys.stderr)
        return 2
    print(f"clang views: units {stats['units_default']} default / {stats['units_release']} release / "
          f"{stats['units_not_rel']} NOT-REL, files entered {stats['files_entered']}, removed names {len(removed)}")
    comps = stats['components']
    print(f"link closure of {stats['exe']} (each build's own graph): " + '; '.join(
        f"{v} {', '.join(f'{c} {n}' for c, n in sorted(comps[v].items()))}" for v in ('default', 'release')))
    dropped = stats['dropped_by_release']
    try:
        oracle = cmake_list(read(os.path.join(a.root, 'CMakeLists.txt')), 'ORACLE_SOURCES')
        other = [f for f in dropped if f not in oracle]
        note = f'ORACLE_SOURCES: {len(dropped) - len(other)}' + (f'; not in ORACLE_SOURCES: {" ".join(other)}'
                                                                 if other else '')
    except (OSError, Unmodelled) as e:
        note = f'ORACLE_SOURCES not read: {e}'
    print(f'units the release graph does not link (its SR_ORACLE=0 view of them is empty): {len(dropped)} ({note})'
          + (f"; units only the release links: {' '.join(stats['added_by_release'])}"
             if stats['added_by_release'] else ''))
    print('\n'.join(report(removed, surviving)))
    # Files no processed unit enters (headers no unit includes, sources no target builds): what the lexer reads in
    # them is reported, never decided.
    unreachable = {}
    for f in scan_files(a.root):
        if f in entered:
            continue
        try:
            both = view_names(read(os.path.join(a.root, f)), f)
            names = sorted(both[0] | both[1])
        except Unmodelled as e:
            names = [f'(the lexer cannot read it: {e})']
        if names:
            unreachable[f] = names
    print(f'unreachable (no processed unit enters them), with SR_* literals: {len(unreachable)} file(s)'
          + (': ' + '; '.join(f'{f} {" ".join(n)}' for f, n in unreachable.items()) if unreachable else ''))
    # The lexer as a fast advisory pre-check: its result is compared, never used.
    try:
        _, _, lex_removed, lex_surviving, _ = sweep(a.root)
        diff_r = sorted(set(removed) ^ set(lex_removed))
        diff_s = sorted(set(surviving) ^ set(lex_surviving))
        advisory = {'removed': len(lex_removed), 'surviving': len(lex_surviving),
                    'removed_differs': diff_r, 'surviving_differs': diff_s}
        print(f'advisory lexer: removed {len(lex_removed)}, surviving {len(lex_surviving)}; names that differ from '
              f'clang: removed {" ".join(diff_r) or "none"}; surviving {" ".join(diff_s) or "none"}')
    except (OSError, Unmodelled) as e:
        advisory = {'error': str(e)}
        print(f'advisory lexer: could not read the tree ({e}) - clang decided')
    # What this tool does not examine (Astra round 8): reported, never skipped; a person acknowledges each one.
    unack = sorted(k for k in uncovered if k not in acks)
    stale = sorted(acks - set(uncovered))
    print(f'UNCOVERED inputs of {stats["exe"]} ({UNCOVERED_AUTHORITY}): {len(uncovered)}, '
          f'not acknowledged {len(unack)}')
    for k in sorted(uncovered):
        u = uncovered[k]
        print(f'  {k}\t[{", ".join(u["views"])}]\t{u["detail"]}\t'
              f'{"acknowledged" if k in acks else "NOT ACKNOWLEDGED"}')
    if stale:
        print(f'--uncovered-ok items that are no longer uncovered: {"; ".join(stale)}')
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as fh:
            json.dump({'mode': 'clang', 'stats': stats, 'removed': removed, 'surviving': surviving,
                       'unreachable': unreachable,
                       'uncovered': [dict(item=k, acknowledged=k in acks, **uncovered[k]) for k in sorted(uncovered)],
                       'uncovered_authority': UNCOVERED_AUTHORITY, 'uncovered_not_acknowledged': unack,
                       'uncovered_ok_stale': stale, 'advisory_lexer': advisory}, fh, indent=1)
    if unack:
        print(f'gate_sweep: {len(unack)} UNCOVERED input(s) not acknowledged (--uncovered-ok lists the ones a person '
              f'has judged; exe_sweep on the built binary is their authority): {"; ".join(unack)}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
