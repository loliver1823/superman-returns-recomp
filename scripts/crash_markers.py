#!/usr/bin/env python3
"""crash_markers.py -- SINGLE SOURCE OF TRUTH for crash-marker patterns.

Both parse_crash_log.py (the authoritative crash parser) and smoke_test.py
(the L2 regression gate) import their crash-detection regexes from here so the
two can never diverge. Add a new marker once, here, and both tools see it.

All patterns are documented in CLAUDE.md "Crash log format reference".

ASCII-only. No unicode in any string a console might print.
"""

import re

# ---------------------------------------------------------------------------
# FATAL crash markers.
#
# Each entry: (name, compiled_regex). The regex is the SAME text both tools use:
#   - parse_crash_log.py uses the capture groups to pull register/context detail.
#   - smoke_test.py only needs to know "did any fatal marker appear".
#
# Deliberately narrow so the benign handled exceptions are NOT flagged:
#   - divw/divwu zero-guard VEH (handled, ~21K/sec at one point)
#   - RtlRaiseException used for SetThreadName (benign)
# Those never use the "Guest harness: exception", "[CRASH] Exception",
# "[ExCreateThread] ... exception code=", or "[BUGCHECK] Halting" phrasings.
# ---------------------------------------------------------------------------

# Main guest exception (access violation mid-execution).
#   Guest harness: exception 0xCODE at host IP 0xADDR
GUEST_HARNESS_EXCEPTION = re.compile(
    r'Guest harness: exception (0x[0-9A-Fa-f]+) at host IP (0x[0-9A-Fa-f]+)'
)

# Spawned guest-thread exception.
#   [ExCreateThread] guest thread startVA=0x.. exception code=0x.. at host=.. fault=0x.. lr=0x.. r3=0x.. r1=0x..
EXCREATETHREAD_CRASH = re.compile(
    r'\[ExCreateThread\] guest thread startVA=(0x[0-9A-Fa-f]+) '
    r'exception code=(0x[0-9A-Fa-f]+) at host=([0-9A-Fa-f]+) '
    r'fault=(0x[0-9A-Fa-f]+).*?'
    r'lr=(0x[0-9A-Fa-f]+).*?'
    r'r3=(0x[0-9A-Fa-f]+).*?'
    r'r1=(0x[0-9A-Fa-f]+)'
)

# iter-449o bake-audit finding: the live guest-thread-death phrasing is
# [THREAD_FATAL], which the [ExCreateThread] pattern above no longer matches --
# a guest-thread death without a process crash was evading smoke_test.
#   [THREAD_FATAL] guest thread startVA=0x.. exception code=0x.. at host=..
#   imageOfs=0x.. fault=0x.. ctx: lr=0x.. ctr=.. r3=0x.. r1=0x..
THREAD_FATAL = re.compile(
    r'\[THREAD_FATAL\] guest thread startVA=(0x[0-9A-Fa-f]+) '
    r'exception code=(0x[0-9A-Fa-f]+) at host=([0-9A-Fa-f]+).*?'
    r'fault=(0x[0-9A-Fa-f]+).*?'
    r'lr=(0x[0-9A-Fa-f]+).*?'
    r'r3=(0x[0-9A-Fa-f]+).*?'
    r'r1=(0x[0-9A-Fa-f]+)'
)

# Final host-side crash marker.
#   [CRASH] Exception 0xCODE at ADDR (thread N)
FINAL_CRASH = re.compile(
    r'\[CRASH\] Exception (0x[0-9A-Fa-f]+) at ([0-9A-Fa-f]+) '
    r'\(thread (\d+)\)'
)

# Explicit harness bugcheck/halt.
#   [BUGCHECK] Halting ...
BUGCHECK = re.compile(r'\[BUGCHECK\] Halting', re.I)

# Lighter-weight markers that also indicate a fatal stop. These do not carry the
# structured context parse_crash_log unpacks, but they DO mean the run died, so
# smoke_test must treat them as FAIL. Kept here so detection is exhaustive.
#   Unhandled exception ... / terminate called / EXCEPTION_ACCESS_VIOLATION as a host marker line
HOST_UNHANDLED = re.compile(
    r'(Unhandled exception|terminate called after throwing|'
    r'EXCEPTION_ACCESS_VIOLATION|Fatal error)', re.I
)

# iter-463 (burndown item 10): an unresolvable guest indirect call. The default
# exe logs [ICALL_UNRESOLVED] (call site, target, object) and stops with this
# marker instead of fabricating r3=0 (kernel/indirect_call.cpp).
#   [ICALL_FAIL] unresolvable guest indirect call: target=.. call_site=..
# The same terminal path writes a shorter marker to stderr. Either stream may
# be the only surviving receipt. ICALL_UNRESOLVED alone is NOT fatal: the
# opt-in legacy forcing path logs it and continues without ICALL_FAIL.
#   [ICALL_FAIL] target=.. call_site=..
ICALL_FAIL = re.compile(
    r'\[ICALL_FAIL\] (?:unresolvable guest indirect call: )?'
    r'target=([0-9A-Fa-f]{8}) call_site=([0-9A-Fa-f]{8})\b'
)

# Ordered list of (name, regex) used by smoke_test's PASS/FAIL detection.
# parse_crash_log references the individual named patterns above directly.
FATAL_MARKERS = [
    ("guest_harness_exception", GUEST_HARNESS_EXCEPTION),
    ("excreatethread_crash", EXCREATETHREAD_CRASH),
    ("thread_fatal", THREAD_FATAL),
    ("final_crash", FINAL_CRASH),
    ("bugcheck", BUGCHECK),
    ("icall_fail", ICALL_FAIL),
    ("host_unhandled", HOST_UNHANDLED),
]

# ---------------------------------------------------------------------------
# Progress / liveness markers (not fatal, but feed the HANG / LOW-PROGRESS
# verdicts in smoke_test).
# ---------------------------------------------------------------------------

# [D3D_DIAG] present #N -- the north-star progress counter.
PRESENT = re.compile(r'\[D3D_DIAG\] present #(\d+)')

# A bare "present #N" also appears in some lines; smoke_test's floor check uses
# the looser form historically, so expose both.
PRESENT_LOOSE = re.compile(r'present #(\d+)')

# Walker watchdog -- the FM module walker stalled. Strong hang signal.
WALKER_WATCHDOG = re.compile(r'\[WALKER_WATCHDOG\]', re.I)

# Watchpoint hits -- informational, not fatal on their own.
WATCHPOINT = re.compile(r'\[WATCHPOINT\]')

# Milestone markers.
WALK_END = re.compile(r'\[FM_CTOR\] WALK_END')
CTOR_END = re.compile(r'\[FM_CTOR\] END')


def scan_fatal(line):
    """Return the name of the first fatal marker matching `line`, else None."""
    for name, rx in FATAL_MARKERS:
        if rx.search(line):
            return name
    return None
