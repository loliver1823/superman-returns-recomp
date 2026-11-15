"""audit_log_gating.py - is counting this log tag a measurement, or am I
measuring the emitter's own cap?

THE FAILURE THIS EXISTS TO STOP. Most emitters in this tree are gated:
`if (n <= 24)`, `if ((n & 0x3FF) == 0)`, `if (v != last)`,
`if (s_logged.insert(step).second)`. Counting the lines then reports the CAP,
the SAMPLE STRIDE, or the number of DISTINCT values - not the event count.
iter-454d did exactly that: it reported 48 `[APT_REQ]` lines as 48 requests when
the emitter caps at 24 and prints two lines each, and the uncapped gauge said 34.
`kernel/fe_flow_watch.cpp:177` had already recorded that trap as its SIXTH
occurrence, in a comment directly above the cap.

USE IT BEFORE QUOTING ANY NUMBER DERIVED FROM LOG LINES:

    python tools/audit_log_gating.py --tag APT_REQ      # one tag, with source
    python tools/audit_log_gating.py --unsafe           # count-unsafe tags
    python tools/audit_log_gating.py --unproved         # tags with no gate found
    python tools/audit_log_gating.py --safe             # verified-safe counters
    python tools/audit_log_gating.py --selftest         # adversarial fixtures
    python tools/audit_log_gating.py                    # full table + totals

=== EXIT CODES: --tag IS FAIL-CLOSED AND NEVER RETURNS 0 ===

    1  COUNT-UNSAFE   a gate was found; counting lines measures the gate
    2  UNPROVED       no gate found, or a bare counter - NOT ESTABLISHED
    3  NOT RECOGNIZED no emitter for this tag in the trees scanned

There is deliberately NO success code for `--tag`. This tool can prove a tag is
count-unsafe; it can never prove one is count-safe, because "no gate found" is
not "no gate exists". A workflow that treats exit 0 as permission to count would
be wrong for every tag, so exit 0 is not offered. The only counts this project
has established as safe are the six gauges under `--safe`, and those are not
tags. (An earlier revision returned 0 for UNPROVED tags; automation could read
that as approval. Director finding, iter-455b.)

=== WHAT THIS TOOL DOES AND DOES NOT ESTABLISH ===

It is a SCREEN over emitter sites it can RECOGNIZE, not a census of all logging.

  * It recognizes call sites matching SR_LOG_BOOT / logInit2Progress that carry
    a "[TAG]" literal. Other loggers, runtime-built tag strings, and emitters
    outside kernel/ gfx/ guest/ppc are NOT seen at all.
  * Comments are stripped from the WHOLE file, preserving line numbering, before
    either emitter detection or gate detection runs. Both a commented-out
    emitter and a cap mentioned only in prose are therefore ignored.
  * Gate detection is still regex; it can miss an exotic gate.
  * `UNGATED?` / `COUNTED-UNCAPPED?` mean NOT ESTABLISHED, and both exit 2.

The `--safe` list is the set of counters EXPLICITLY VERIFIED uncapped by reading
their source. It is not a proof that nothing else is safe.
"""
import argparse
import collections
import os
import re
import sys
import tempfile

# The tool must not crash under the default Windows cp1252 console: every string
# it prints is ASCII, and the streams are reconfigured to replace rather than
# raise if anything slips through. (Director finding, iter-455.)
for _s in ('stdout', 'stderr'):
    try:
        getattr(sys, _s).reconfigure(encoding=getattr(sys, _s).encoding or 'utf-8',
                                     errors='replace')
    except Exception:                                          # noqa: BLE001
        pass

ROOTS = ['kernel', 'gfx', 'guest/ppc']
EXTS = ('.cpp', '.hpp', '.h')
LOOKBACK = 14

TAG_RE = re.compile(r'"\s*\[([A-Z0-9_]{2,})\]')
EMIT_RE = re.compile(r'SR_LOG_BOOT|logInit2Progress')

EXIT_UNSAFE, EXIT_UNPROVED, EXIT_UNRECOGNIZED = 1, 2, 3


def strip_comments_file(src):
    """Remove // and /* */ text from a WHOLE source, preserving line count.

    Two director findings made this whole-file rather than per-window:
      * block-comment state was lost at the lookback boundary, so a `/*` opened
        earlier left `n <= 24` inside a comment classified as a real cap;
      * emitter detection ran on RAW lines, so a commented-out
        `// SR_LOG_BOOT(..., "[FAKE_TAG] ...")` was counted as a real emitter.
    String literals are tracked so a `//` inside "http://..." is not treated as
    a comment. Newlines are preserved exactly so file:line stays correct.
    """
    out = []
    i, n = 0, len(src)
    in_line = in_block = in_str = in_chr = False
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if in_line:
            if c == '\n':
                in_line = False
                out.append(c)
            else:
                out.append(' ')
            i += 1
        elif in_block:
            if c == '*' and nxt == '/':
                in_block = False
                out.append('  ')
                i += 2
            else:
                out.append('\n' if c == '\n' else ' ')
                i += 1
        elif in_str or in_chr:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(nxt)
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
            i += 1
        else:
            if c == '/' and nxt == '/':
                in_line = True
                out.append('  ')
                i += 2
            elif c == '/' and nxt == '*':
                in_block = True
                out.append('  ')
                i += 2
            elif c == '"':
                in_str = True
                out.append(c)
                i += 1
            elif c == "'":
                in_chr = True
                out.append(c)
                i += 1
            else:
                out.append(c)
                i += 1
    return ''.join(out)


PATTERNS = [
    (re.compile(r'\.insert\([^)]*\)\s*\.second'), 'CHANGE-TRIGGERED',
     'first-time-only per key (set insert) - count is DISTINCT KEYS, not events'),
    (re.compile(r'!=\s*t_last|!=\s*s_last|!=\s*last[A-Z_]\w*|!=\s*prev[A-Z_]\w*'),
     'CHANGE-TRIGGERED',
     'emits only on a CHANGE - count is transitions, not events'),
    (re.compile(r'%\s*(\d+)\s*\)\s*==\s*0'), 'SAMPLED',
     'every Nth call only - line count x N approximates the population'),
    (re.compile(r'&\s*0x([0-9A-Fa-f]+)u?\s*\)\s*==\s*0'), 'SAMPLED',
     'mask stride - line count x (mask+1) approximates the population'),
    (re.compile(r'<=\s*(\d+)u?\b'), 'CAPPED',
     'first N only - counting lines reports the CAP'),
    (re.compile(r'<\s*(\d+)u?\s*\)'), 'CAPPED',
     'first N only - counting lines reports the CAP'),
    (re.compile(r'fetch_add\(\s*1'), 'COUNTED',
     'a counter exists; check whether it is bumped before or inside the gate'),
]

# STRUCTURED METADATA, not prose. Every field below is asserted against the
# checkout by --selftest group H, so this table cannot drift from the code and
# cannot present a non-entry address as a hook.
#
# WHY IT IS STRUCTURED: this table previously said aptPump was `sub_826D27C8`.
# That address is NOT a function - it is entry+0xB0, the link register after the
# call to AptExecuteFrameInContext at 0x826D27C4, and `guest/ppc/ppc_recomp.91.cpp`
# literally contains `ctx.lr = 0x826D27C8;`. `kernel/natural_gauges.cpp:101-109`
# had already written that down ("a RETURN address 176 bytes INTO
# AptExecutorPump_sub_826D2718 ... hooking it fails to link"), while the enum
# comment in natural_gauges.hpp still carried the wrong value and this tool
# copied it. A tool that exists to stop bad attribution was publishing one.
#
# hook kinds:
#   strong_def   - kernel defines sub_<VA> and tail-calls __imp__sub_<VA>
#   lookup_shim  - kernel installs a shim into PPC_LOOKUP_FUNC(gb, <VA>) and
#                  calls the saved original. NOT every gauge is a strong-def.
GAUGES = [
    dict(name='feTick', va='821494C0', kind='strong_def',
         src='kernel/natural_gauges.cpp', wrapper='sub_821494C0',
         passthrough='__imp__sub_821494C0', enum='FeTick',
         meaning='FE master tick, natural calls only'),
    dict(name='aptPump', va='826D2718', kind='strong_def',
         src='kernel/apt_parse_trace.cpp', wrapper='sub_826D2718',
         passthrough='__imp__sub_826D2718', enum='AptPump',
         meaning='Apt executor pump tick; calls AptDrainCompleted_826D23A0 '
                 'unconditionally at 0x826D279C'),
    dict(name='clipCtorAll', va='826B0458', kind='strong_def',
         src='kernel/cs_timeout_underflow_fix.cpp', wrapper='sub_826B0458',
         passthrough='__imp__sub_826B0458', enum='ClipCtorAll',
         meaning='every type-12 clip ctor'),
    dict(name='clipCtorReal', va='826B0458', kind='strong_def',
         src='kernel/cs_timeout_underflow_fix.cpp', wrapper='sub_826B0458',
         passthrough='__imp__sub_826B0458', enum='ClipCtorReal',
         shares_entry_with='clipCtorAll',
         meaning='same ctor with movieDef != NULL - a REAL movie instance'),
    dict(name='defExec', va='826B2298', kind='strong_def',
         src='kernel/cs_timeout_underflow_fix.cpp', wrapper='sub_826B2298',
         passthrough='__imp__sub_826B2298', enum='DefExec',
         meaning='begin movie-def processing'),
    dict(name='aptReq', va='826D2690', kind='lookup_shim',
         src='kernel/fe_flow_watch.cpp', wrapper='shimAptLoadByPath',
         passthrough='s_orig2690', enum='AptReq',
         meaning='load-by-name request (uncapped, unlike the capped [APT_REQ] log)'),
]

# iter-463 phase 2 (docs/DEVTOOLS_DESIGN.md 3.1/3.2, A7): the run-tag lines are REGISTERED as
# uncapped by design - every emitter logs once per event with no cap, and --selftest group K
# fails if a cap or a new emitter file appears. They are TAGS (presence decides the run class),
# not counters: the count of record for dev-tool actions is the [GAUGE] devtoolActs field, an
# uncapped atomic. --tag keeps its fail-closed exit codes for them (never 0).
REGISTERED_TAGS = [
    dict(tag='RUN_CLASS', files=('kernel/devtools.cpp', 'gfx/d3d12_backend.cpp'),
         rule='one line per tagging event, never capped: the boot tag, save-seeded, a marked LOAD, '
              'a marker write, the present-dump sidecar',
         of_record='PRESENCE, not count: any [RUN_CLASS] other than natural+hud makes the run '
                   'non-natural (tools/verify_run.py, run_recomp_x runClass)'),
    dict(tag='DEVTOOL', files=('kernel/devtools.cpp', 'kernel/devtools_levelskip.cpp'),
         rule='one line per executor event / action / refusal, never capped',
         of_record='[GAUGE] devtoolActs= (uncapped atomic, every census line) is the action count'),
]

UNSAFE = ('CAPPED', 'SAMPLED', 'CAPPED+SAMPLED', 'CHANGE-TRIGGERED')
UNPROVED = ('COUNTED-UNCAPPED?', 'UNGATED?')


def scan(roots=None):
    rows = []
    for root in (roots or ROOTS):
        if not os.path.isdir(root):
            continue
        for dp, _dn, fns in os.walk(root):
            for fn in fns:
                if not fn.endswith(EXTS):
                    continue
                p = os.path.join(dp, fn)
                try:
                    raw = open(p, encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                clean = strip_comments_file(raw).split('\n')
                for i, ln in enumerate(clean):
                    if not EMIT_RE.search(ln):
                        continue
                    tm = TAG_RE.search('\n'.join(clean[i:i + 3]))
                    if not tm:
                        continue
                    ctx = clean[max(0, i - LOOKBACK):i + 1]
                    hits = []
                    for rx, cls, why in PATTERNS:
                        for cl in ctx:
                            m = rx.search(cl)
                            if m:
                                n2 = m.group(1) if m.groups() else ''
                                hits.append((cls, why, n2, cl.strip()))
                                break
                    rows.append(dict(tag=tm.group(1), file=p.replace('\\', '/'),
                                     line=i + 1, hits=hits))
    return rows


def verdict(entries):
    classes = {h[0] for e in entries for h in e['hits']}
    if 'CHANGE-TRIGGERED' in classes:
        return 'CHANGE-TRIGGERED'
    if 'SAMPLED' in classes and 'CAPPED' in classes:
        return 'CAPPED+SAMPLED'
    if 'SAMPLED' in classes:
        return 'SAMPLED'
    if 'CAPPED' in classes:
        return 'CAPPED'
    if 'COUNTED' in classes:
        return 'COUNTED-UNCAPPED?'
    return 'UNGATED?'


def group(rows):
    bytag = collections.OrderedDict()
    for r in rows:
        bytag.setdefault(r['tag'], []).append(r)
    return bytag


def cmd_tag(a, bytag):
    t = a.tag.strip().strip('[]').upper()
    entries = bytag.get(t)
    if not entries:
        print('[%s] -> NOT RECOGNIZED (exit %d)' % (t, EXIT_UNRECOGNIZED))
        print('No emitter for this tag in %s.' % ', '.join(ROOTS))
        print('It may use a logger this tool does not recognize, be built from a')
        print('runtime string, or live elsewhere. This is NOT a safe result.')
        return EXIT_UNRECOGNIZED
    v = verdict(entries)
    rc = EXIT_UNSAFE if v in UNSAFE else EXIT_UNPROVED
    print('[%s]  ->  %s   (exit %d)\n' % (t, v, rc))
    for reg in REGISTERED_TAGS:
        if reg['tag'] == t:
            print('  REGISTERED UNCAPPED TAG (DEVTOOLS_DESIGN 3.1 A7): %s' % reg['rule'])
            print('  of record: %s\n' % reg['of_record'])
    for e in entries:
        print('  %s:%d' % (e['file'], e['line']))
        for cls, why, n, srcline in e['hits']:
            tail = ' (N=%s)' % n if n else ''
            print('      %s%s: %s' % (cls, tail, why))
            print('        %s' % srcline)
    if rc == EXIT_UNSAFE:
        print('\n*** COUNTING [%s] LINES DOES NOT MEASURE THE EVENT. ***' % t)
        if v == 'CHANGE-TRIGGERED':
            print('    Its count is transitions or distinct keys. Silence IS')
            print('    meaningful (the value did not change) but the COUNT is not.')
    else:
        print('\n*** NOT ESTABLISHED for [%s]. ***' % t)
        print('    "No gate found" is not "no gate exists", and a bare counter is')
        print('    a lead, not a guarantee. Read the site above before counting.')
    print('    Use a verified counter (--safe), or the emitter\'s own highest #N')
    print('    ordinal as a lower bound, or say "log lines" and mean it.')
    return rc


# ---------------------------------------------------------------- fixtures --

FIXTURES = [
    ('f_cap.cpp', '''
void f() {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1) + 1;
  if (n <= 24) {
    SR_LOG_BOOT(Info, "[F_CAP] #%u", n);
  }
}
''', 'F_CAP', 'CAPPED'),

    ('f_sampled.cpp', '''
void f() {
  static std::atomic<uint32_t> s_a{0};
  const uint32_t n = s_a.fetch_add(1) + 1;
  if ((n & 0x3FF) == 0) {
    SR_LOG_BOOT(Info, "[F_SAMPLED] #%u", n);
  }
}
''', 'F_SAMPLED', 'SAMPLED'),

    ('f_both.cpp', '''
void f() {
  static std::atomic<uint32_t> s_a{0};
  const uint32_t n = s_a.fetch_add(1) + 1;
  if (n <= 64 || (n & 0x3FF) == 0) {
    SR_LOG_BOOT(Info, "[F_BOTH] #%u", n);
  }
}
''', 'F_BOTH', 'CAPPED+SAMPLED'),

    ('f_change.cpp', '''
void f(uint32_t v) {
  static uint32_t t_last = 0xFFFFFFFFu;
  if (v != t_last) {
    t_last = v;
    SR_LOG_BOOT(Info, "[F_CHANGE] v=%u", v);
  }
}
''', 'F_CHANGE', 'CHANGE-TRIGGERED'),

    ('f_setinsert.cpp', '''
void f(const std::string& step) {
  static std::set<std::string> s_logged;
  if (s_logged.insert(step).second)
    logInit2Progress("[F_SETINSERT] %s", step.c_str());
}
''', 'F_SETINSERT', 'CHANGE-TRIGGERED'),

    ('f_comment.cpp', '''
void f() {
  // a reader counting these lines would report the cap as the measurement,
  // because a naive gate would be n <= 24 here. It is not.
  /* another block comment mentioning (n & 0x3FF) == 0 to be safe */
  SR_LOG_BOOT(Info, "[F_COMMENT] always");
}
''', 'F_COMMENT', 'UNGATED?'),

    ('f_counter.cpp', '''
void f() {
  static std::atomic<uint32_t> s_n{0};
  s_n.fetch_add(1);
  SR_LOG_BOOT(Info, "[F_COUNTER] every time");
}
''', 'F_COUNTER', 'COUNTED-UNCAPPED?'),

    # Director finding 2: the block comment OPENS far above the lookback window.
    ('f_farblock.cpp', '/*\n' + '\n'.join(
        ' * filler line %d so the opening delimiter is outside LOOKBACK' % i
        for i in range(40)) + '''
 * a naive gate would be n <= 24 here, but this is all comment text
 */
void f() {
  SR_LOG_BOOT(Info, "[F_FARBLOCK] always");
}
''', 'F_FARBLOCK', 'UNGATED?'),
]

# Director finding 3: emitters that exist only inside comments must NOT be
# recognized at all. Director finding 5: an unsupported logger likewise.
NEGATIVE_FIXTURES = [
    ('f_commented_emitter.cpp',
     '// SR_LOG_BOOT(Info, "[FAKE_LINE_TAG] never real");\n',
     'FAKE_LINE_TAG'),
    ('f_blocked_emitter.cpp',
     '/*\nSR_LOG_BOOT(Info, "[FAKE_BLOCK_TAG] never real");\n*/\n',
     'FAKE_BLOCK_TAG'),
    ('f_unknown_logger.cpp',
     'void f() {\n  MY_OTHER_LOGGER(Info, "[UNKNOWN_LOGGER_TAG] not our logger");\n}\n',
     'UNKNOWN_LOGGER_TAG'),
]


def ppc_entry_exists(va):
    """Is <va> a generated PPC FUNCTION ENTRY, not merely an address inside one?

    The recompiler emits `PPC_FUNC_IMPL(__imp__sub_<VA>)` for every function it
    generates, so a hookable entry has that marker and a return address does
    not. Hermetic: reads the checkout only - no IDA, no MCP, no cached VA index
    (a stale .cache/va_index.db must never be able to bless an address).
    """
    marker = 'PPC_FUNC_IMPL(__imp__sub_%s)' % va.upper()
    root = os.path.join('guest', 'ppc')
    if not os.path.isdir(root):
        return False
    for dp, _dn, fns in os.walk(root):
        for fn in fns:
            if not fn.endswith('.cpp'):
                continue
            try:
                if marker in open(os.path.join(dp, fn), encoding='utf-8',
                                  errors='replace').read():
                    return True
            except OSError:
                continue
    return False


def main_safe():
    """Render the --safe table. Split out so the selftest can assert its text."""
    print('COUNTERS EXPLICITLY VERIFIED UNCAPPED (kernel/natural_gauges.hpp).')
    print('Read them from the [GAUGE] census line.')
    print('VERIFIED-SAFE, not EXCLUSIVE: other safe counters may exist that')
    print('this tool has not proved. These are counters, not log tags.')
    print('Every field below is asserted against the checkout by --selftest.\n')
    print('  %-13s %-10s %-12s %s' % ('GAUGE', 'ENTRY', 'HOOK', 'HOOK SITE'))
    for g in GAUGES:
        shared = ('  [shares entry with %s]' % g['shares_entry_with']
                  if g.get('shares_entry_with') else '')
        print('  %-13s sub_%-6s %-12s %s :: %s%s'
              % (g['name'], g['va'], g['kind'], g['src'], g['wrapper'], shared))
        print('  %-13s   %s' % ('', g['meaning']))
    print('\nNOTE: 0x826D27C8 is NOT a function. It is aptPump entry +0xB0, the')
    print('link register after the call at 0x826D27C4. It appears in older')
    print('notes because a watchpoint backtrace named a call SITE; you hook the')
    print('containing function. See kernel/natural_gauges.cpp:101-109.')
    print('\nREGISTERED UNCAPPED TAGS (presence tags, not counters; --selftest group K):')
    for reg in REGISTERED_TAGS:
        print('  [%s]  %s' % (reg['tag'], reg['rule']))
        print('  %-*s  of record: %s' % (len(reg['tag']) + 2, '', reg['of_record']))
    print('\nEvery TAG needs --tag <TAG> first, and no tag ever returns 0.')


def cmd_selftest(_a):
    import io
    import subprocess
    ran, failures = [], []

    def check(name, cond, detail=''):
        ran.append(name)
        if cond:
            print('  PASS  %s' % name)
        else:
            print('  FAIL  %s  %s' % (name, detail))
            failures.append(name)

    repo_before = sorted(g for g in os.listdir('.')
                         if g.startswith('loggate_'))
    tmp_root = tempfile.gettempdir()
    tmp_before = sorted(g for g in os.listdir(tmp_root)
                        if g.startswith('loggate_'))

    # Director finding 1: deterministic cleanup. TemporaryDirectory removes the
    # tree on exit even if an assertion raises.
    with tempfile.TemporaryDirectory(prefix='loggate_selftest_') as tmp:
        d = os.path.join(tmp, 'kernel')
        os.makedirs(d, exist_ok=True)
        for fn, src, _t, _e in FIXTURES:
            open(os.path.join(d, fn), 'w', encoding='utf-8').write(src)
        for fn, src, _t in NEGATIVE_FIXTURES:
            open(os.path.join(d, fn), 'w', encoding='utf-8').write(src)

        # Director finding 1 killer: injected failure AFTER the bank exists.
        # TemporaryDirectory must still remove it on the way out.
        if os.environ.get('SR_LOGGATE_FORCE_FAIL'):
            raise RuntimeError('injected failure: fixture bank exists, testing cleanup')

        bytag = group(scan([tmp]))

        print('A. classification')
        for _fn, _src, tag, expected in FIXTURES:
            got = verdict(bytag[tag]) if tag in bytag else 'MISSING'
            check('A %-14s -> %-18s' % (tag, expected), got == expected,
                  'got %s' % got)

        print('\nB. comment-only emitters and unknown loggers are NOT recognized')
        for _fn, _src, tag in NEGATIVE_FIXTURES:
            check('B %-22s not recognized' % tag, tag not in bytag,
                  'tag was recognized as an emitter')

        print('\nC. exit codes are fail-closed (--tag never returns 0)')
        for _fn, _src, tag, expected in FIXTURES:
            want = EXIT_UNSAFE if expected in UNSAFE else EXIT_UNPROVED
            rc = quiet_tag(tag, bytag)
            check('C %-14s exit %d' % (tag, want), rc == want, 'rc=%d' % rc)
        check('C unknown tag exit %d' % EXIT_UNRECOGNIZED,
              quiet_tag('NO_SUCH_TAG', bytag) == EXIT_UNRECOGNIZED)
        check('C no fixture tag returns 0',
              all(quiet_tag(t, bytag) != 0 for t in bytag))

        print('\nD. ASCII / cp1252 output')
        buf = io.StringIO()
        old = sys.stdout
        try:
            sys.stdout = buf
            for t in ('F_CAP', 'F_COMMENT', 'NO_SUCH_TAG'):
                cmd_tag(argparse.Namespace(tag=t), bytag)
            print_table(bytag, False, False)
        finally:
            sys.stdout = old
        text = buf.getvalue()
        check('D output is pure ASCII', all(ord(c) < 128 for c in text))
        try:
            text.encode('cp1252')
            check('D output encodes as cp1252', True)
        except UnicodeEncodeError as e:
            check('D output encodes as cp1252', False, str(e))

        # Director finding 4: assert the REAL CLI, not only internal classes.
        # This runs INSIDE the unique TemporaryDirectory: there is no fixed
        # shared path and no rmtree of one, so two selftests cannot collide and
        # nothing survives an exception. (The previous revision copied the bank
        # to a fixed tempdir/loggate_cli_root, which leaked on exception and
        # made concurrent runs delete each other's fixtures.)
        print('\nF. real CLI exit codes (subprocess, not class membership)')
        for tag, want in (('F_CAP', EXIT_UNSAFE),
                          ('F_CHANGE', EXIT_UNSAFE),
                          ('F_BOTH', EXIT_UNSAFE),
                          ('F_COMMENT', EXIT_UNPROVED),
                          ('F_COUNTER', EXIT_UNPROVED),
                          ('F_FARBLOCK', EXIT_UNPROVED),
                          ('FAKE_LINE_TAG', EXIT_UNRECOGNIZED),
                          ('FAKE_BLOCK_TAG', EXIT_UNRECOGNIZED),
                          ('UNKNOWN_LOGGER_TAG', EXIT_UNRECOGNIZED),
                          ('DEFINITELY_NOT_A_TAG', EXIT_UNRECOGNIZED)):
            r = subprocess.run([sys.executable, __file__, '--root', tmp,
                                '--tag', tag], capture_output=True)
            check('F CLI --tag %-20s exit %d' % (tag, want),
                  r.returncode == want,
                  'rc=%d out=%r' % (r.returncode, r.stdout[:100]))
        r = subprocess.run([sys.executable, __file__, '--root', tmp],
                           capture_output=True)
        check('F CLI table exit 0', r.returncode == 0, 'rc=%d' % r.returncode)

        # Director addendum: the --safe table must be source-backed, so a
        # return address can never again be published as a hook.
        print('\nH. --safe metadata is backed by the checkout')
        for g in GAUGES:
            entry = 'PPC_FUNC_IMPL(__imp__sub_%s)' % g['va']
            found = ppc_entry_exists(g['va'])
            check('H %-13s sub_%s is a generated PPC ENTRY' % (g['name'], g['va']),
                  found, 'no %s in guest/ppc' % entry)
            try:
                src = open(g['src'], encoding='utf-8', errors='replace').read()
            except OSError as e:
                check('H %-13s hook source readable' % g['name'], False, str(e))
                continue
            if g['kind'] == 'strong_def':
                ok = ('void %s(' % g['wrapper']) in src
                how = 'strong-def wrapper void %s(' % g['wrapper']
            else:
                ok = ('PPC_LOOKUP_FUNC' in src
                      and ('0x%su' % g['va']) in src
                      and g['wrapper'] in src)
                how = 'lookup-table install of %s at 0x%s' % (g['wrapper'], g['va'])
            check('H %-13s %s' % (g['name'], g['kind']), ok, 'expected ' + how)
            check('H %-13s bumps Gauge::%s' % (g['name'], g['enum']),
                  ('Gauge::%s' % g['enum']) in src)
            check('H %-13s passes through via %s' % (g['name'], g['passthrough']),
                  g['passthrough'] in src)
        shared = [g for g in GAUGES if g.get('shares_entry_with')]
        check('H shared-entry gauges are explicit (clipCtorAll/clipCtorReal)',
              len(shared) == 1 and shared[0]['name'] == 'clipCtorReal'
              and shared[0]['shares_entry_with'] == 'clipCtorAll')
        vas = [g['va'] for g in GAUGES]
        check('H the only duplicate entry VA is the declared shared one',
              len(vas) - len(set(vas)) == len(shared))
        check('H no gauge advertises the 826D27C8 return address',
              not any(g['va'].upper() == '826D27C8' for g in GAUGES))
        buf2 = io.StringIO()
        old2 = sys.stdout
        try:
            sys.stdout = buf2
            main_safe()
        finally:
            sys.stdout = old2
        safe_out = buf2.getvalue()
        check('H --safe is rendered from the metadata (aptPump entry shown)',
              'sub_826D2718' in safe_out)
        check('H --safe output is pure ASCII',
              all(ord(c) < 128 for c in safe_out))

    # Director finding 1: prove the fixture bank is gone.
    #
    # OWN-DIRECTORY assertions first, and they hold under concurrency. A global
    # before/after listing does NOT: my own group J caught this by failing a
    # child that had merely observed its SIBLING's legitimate temp directory and
    # reported it as a leak. The global sweep is still the stronger check for a
    # FIXED-path artifact (the loggate_cli_root defect), so it is kept - but only
    # where no sibling can be running.
    print('\nE. no fixture directory survives (finding 1)')
    check('E my own fixture dir is gone', not os.path.isdir(tmp),
          'still present: %s' % tmp)
    for fixed in ('loggate_cli_root',):
        check('E no fixed-path artifact %s (repo)' % fixed,
              not os.path.isdir(os.path.join('.', fixed)))
        check('E no fixed-path artifact %s (temp)' % fixed,
              not os.path.isdir(os.path.join(tmp_root, fixed)))
    if os.environ.get('SR_LOGGATE_CHILD'):
        print('  (global sweep skipped: a sibling selftest may hold its own dir)')
    else:
        repo_after = sorted(g for g in os.listdir('.')
                            if g.startswith('loggate_'))
        tmp_after = sorted(g for g in os.listdir(tmp_root)
                           if g.startswith('loggate_'))
        check('E repo root gained no loggate_* dir', repo_after == repo_before,
              'before=%s after=%s' % (repo_before, repo_after))
        check('E temp root gained no loggate_* dir', tmp_after == tmp_before,
              'before=%s after=%s' % (tmp_before, tmp_after))

    # K (iter-463 phase 2): the registered run-tag emitters stay uncapped and where they are
    print('\nK. registered uncapped tags (DEVTOOLS_DESIGN 3.1 A7)')
    live = group(scan(['kernel', 'gfx']))
    for reg in REGISTERED_TAGS:
        ents = live.get(reg['tag'], [])
        check('K [%s] has emitters' % reg['tag'], bool(ents))
        check('K [%s] no gate at any emitter (verdict %s)' % (reg['tag'], verdict(ents) if ents else '-'),
              bool(ents) and verdict(ents) == 'UNGATED?')
        files = sorted({e['file'] for e in ents})
        check('K [%s] emitters only in %s' % (reg['tag'], ', '.join(reg['files'])),
              all(f in reg['files'] for f in files), 'found %s' % files)
    g = open(os.path.join('kernel', 'natural_gauges.cpp'), encoding='utf-8', errors='replace').read()
    check('K [GAUGE] carries devtools=<mask> devtoolActs=<n>', 'devtools=0x%X devtoolActs=%llu' in g)
    # a planted cap on a registered emitter is caught
    with tempfile.TemporaryDirectory(prefix='loggate_k_') as tk:
        os.makedirs(os.path.join(tk, 'kernel'))
        open(os.path.join(tk, 'kernel', 'devtools.cpp'), 'w').write(
            'void f(unsigned n) {\n  if (n <= 24)\n    SR_LOG_BOOT(sr::LogSeverity::Warn, "[RUN_CLASS] x");\n}\n')
        capped = group(scan([os.path.join(tk, 'kernel')]))
        check('K a planted cap on [RUN_CLASS] is CAPPED (the K check would fail)',
              verdict(capped.get('RUN_CLASS', [])) == 'CAPPED')

    if not os.environ.get('SR_LOGGATE_CHILD'):
        # Director finding 1: an INJECTED EXCEPTION must still clean up.
        print('\nI. injected-exception cleanup (finding 1 killer)')
        env = dict(os.environ, SR_LOGGATE_CHILD='1', SR_LOGGATE_FORCE_FAIL='1')
        r = subprocess.run([sys.executable, __file__, '--selftest'],
                           capture_output=True, env=env)
        leaked_repo = [g for g in os.listdir('.') if g.startswith('loggate_')]
        leaked_tmp = [g for g in os.listdir(tmp_root) if g.startswith('loggate_')]
        check('I forced-failure child exited nonzero', r.returncode != 0,
              'rc=%d' % r.returncode)
        check('I forced failure left no repo dir', leaked_repo == repo_before,
              'leaked %s' % leaked_repo)
        check('I forced failure left no temp dir', leaked_tmp == tmp_before,
              'leaked %s' % leaked_tmp)

        # Director finding 2: two concurrent selftests must BOTH succeed.
        print('\nJ. two-process concurrent selftest (finding 2 killer)')
        cenv = dict(os.environ, SR_LOGGATE_CHILD='1')
        procs = [subprocess.Popen([sys.executable, __file__, '--selftest'],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=cenv) for _ in range(2)]
        outs = [p.communicate()[0].decode('ascii', 'replace') for p in procs]
        for i, (p, o) in enumerate(zip(procs, outs)):
            check('J concurrent child %d exit 0' % i, p.returncode == 0,
                  'rc=%d tail=%r' % (p.returncode, o[-160:]))
            m = re.search(r'SELFTEST PASSED: (\d+)/(\d+)', o)
            check('J concurrent child %d reports full derived total' % i,
                  bool(m) and m.group(1) == m.group(2),
                  'tail=%r' % o[-160:])
        after_repo = [g for g in os.listdir('.') if g.startswith('loggate_')]
        after_tmp = [g for g in os.listdir(tmp_root) if g.startswith('loggate_')]
        check('J concurrency left no repo dir', after_repo == repo_before,
              'leaked %s' % after_repo)
        check('J concurrency left no temp dir', after_tmp == tmp_before,
              'leaked %s' % after_tmp)

    print('\n%s: %d/%d assertions passed, %d failure(s)'
          % ('SELFTEST PASSED' if not failures else 'SELFTEST FAILED',
             len(ran) - len(failures), len(ran), len(failures)))
    for f in failures:
        print('  - %s' % f)
    return 1 if failures else 0


def quiet_tag(tag, bytag):
    import io
    old = sys.stdout
    try:
        sys.stdout = io.StringIO()
        return cmd_tag(argparse.Namespace(tag=tag), bytag)
    finally:
        sys.stdout = old


def print_table(bytag, unsafe_only, unproved_only):
    print('%-26s %-18s %5s  first site' % ('TAG', 'CLASS', 'SITES'))
    print('-' * 100)
    for tag, entries in sorted(bytag.items()):
        v = verdict(entries)
        if unsafe_only and v not in UNSAFE:
            continue
        if unproved_only and v not in UNPROVED:
            continue
        print('%-26s %-18s %5d  %s:%d'
              % (tag, v, len(entries), entries[0]['file'], entries[0]['line']))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tag', help='report one tag in detail (exit 1/2/3, never 0)')
    ap.add_argument('--unsafe', action='store_true', help='count-unsafe tags only')
    ap.add_argument('--unproved', action='store_true', help='not-established tags only')
    ap.add_argument('--safe', action='store_true',
                    help='counters explicitly verified uncapped')
    ap.add_argument('--selftest', action='store_true',
                    help='adversarial fixtures + assertions')
    ap.add_argument('--root', action='append', default=None,
                    help='scan this tree instead of the defaults (repeatable). '
                         '--selftest uses it to exercise the real CLI against '
                         'fixtures: a full-tree scan takes ~31s, so four '
                         'subprocesses of it would make the selftest unusable.')
    a = ap.parse_args()

    if a.selftest:
        return cmd_selftest(a)

    if a.safe:
        main_safe()
        return 0

    rows = scan(a.root)
    bytag = group(rows)

    if a.tag:
        return cmd_tag(a, bytag)

    print_table(bytag, a.unsafe, a.unproved)
    counts = collections.Counter(verdict(v) for v in bytag.values())
    unsafe = sum(counts[c] for c in UNSAFE)
    unproved = sum(counts[c] for c in UNPROVED)
    print('\nDERIVED TOTALS (this tool produced every figure below):')
    print('  recognized tags       : %d' % len(bytag))
    print('  recognized emit sites : %d' % len(rows))
    print('  COUNT-UNSAFE          : %d   (%s)'
          % (unsafe, ', '.join('%s=%d' % (c, counts[c]) for c in UNSAFE if counts[c])))
    print('  UNPROVED              : %d   (%s)'
          % (unproved, ', '.join('%s=%d' % (c, counts[c]) for c in UNPROVED if counts[c])))
    print('  NOTE: UNPROVED is NOT safe - no gate was found, which is not the')
    print('        same as no gate existing. --tag exits 2 for these, never 0.')
    print('\nVerified-safe counters: --safe. Detail for one tag: --tag <TAG>.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
