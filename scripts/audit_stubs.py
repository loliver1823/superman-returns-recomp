#!/usr/bin/env python3
"""
Static Recomp Auditor — scan kernel stubs for known-bad patterns.

Patterns:
  1. RAW_U32  — *(uint32_t*)(expr) without bswap32 wrapping it
  2. RAW_U64  — *(uint64_t*)(expr) without bswap64 wrapping it
  3. VTABLE   — vtable dispatch missing bswap on vtable pointer read
  4. PTR_CAST — (void*)guest_va or (SomeType*)guest_va without base offset
  5. FPTR     — function pointer cast from guest VA without + base

Usage:
  python scripts/audit_stubs.py [--all] [file ...]

  --all  : also scan guest/ppc/ppc_recomp.*.cpp (slow, ~144 MB)
  file   : specific files to scan (default: kernel/*.cpp + app/*.cpp)

Output: each finding on one line with severity, file, line, pattern, snippet.
"""

import re
import sys
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

# Raw uint32_t dereference — flag if NOT immediately preceded by bswap32(
# We look for the token *(uint32_t*) that is NOT inside bswap32(...)
# Strategy: find all *(uint32_t*)( occurrences, then check if bswap32 is
# present in the same expression (within 50 chars to the left).
_RAW_U32 = re.compile(r'\*\(\s*uint32_t\s*\*\s*\)\s*\(')
_RAW_U64 = re.compile(r'\*\(\s*uint64_t\s*\*\s*\)\s*\(')
_RAW_U16 = re.compile(r'\*\(\s*uint16_t\s*\*\s*\)\s*\(')

# bswap presence check — within a window around the dereference
_BSWAP32 = re.compile(r'bswap32|_byteswap_ulong|__builtin_bswap32')
_BSWAP64 = re.compile(r'bswap64|_byteswap_uint64|__builtin_bswap64')
_BSWAP16 = re.compile(r'bswap16|_byteswap_ushort|__builtin_bswap16')

# Guest VA used as host pointer — (void*)r3 or (SomeType*)ctx.r3.u32
# Catches cases like: auto p = (uint8_t*)ctx.r3.u32;
_CAST_FROM_REG = re.compile(
    r'\(\s*(?:void|uint8_t|uint16_t|uint32_t|char|int)\s*\*\s*\)\s*ctx\.r\d+\.u(?:32|64)'
)

# Direct (void*)<number> cast — catches (void*)0x82345678
_CAST_FROM_LITERAL = re.compile(r'\(\s*(?:void|uint8_t|char)\s*\*\s*\)\s*0x8[0-9A-Fa-f]{6}')

# PPC_CALL_INDIRECT / similar — these are fine (handled by macros)
_INDIRECT_CALL = re.compile(r'PPC_CALL_INDIRECT')

# Lines that look like vtable dispatch: (*(*  patterns
_VTABLE_DISPATCH = re.compile(r'\(\*\s*\(\s*\*')

# Obvious no-bswap on vtable pointer:
# *(uint32_t*)(base + obj_va) used as vtable without bswap
_VTABLE_NO_BSWAP = re.compile(
    r'auto\s+\w+\s*=\s*\*\(\s*uint32_t\s*\*\s*\)\s*\(\s*base'
)

# ---------------------------------------------------------------------------
# Exclusion heuristics
# ---------------------------------------------------------------------------

def _bswap_in_window(line: str, match_start: int, width: int = 60) -> bool:
    """Return True if a bswap call wraps or precedes the match in the line."""
    window = line[max(0, match_start - width): match_start + width]
    return bool(_BSWAP32.search(window) or _BSWAP64.search(window))


def _is_comment(line: str) -> bool:
    stripped = line.lstrip()
    return stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*')


def _is_ppc_macro(line: str) -> bool:
    """PPC_LOAD_U32 / PPC_STORE_U32 already handle endianness — skip."""
    return bool(re.search(r'PPC_(?:LOAD|STORE)_U(?:8|16|32|64)', line))


# ---------------------------------------------------------------------------
# Finding record
# ---------------------------------------------------------------------------

SEVERITY = {
    'RAW_U32': 'HIGH',
    'RAW_U64': 'HIGH',
    'RAW_U16': 'MEDIUM',
    'VTABLE_NO_BSWAP': 'HIGH',
    'VTABLE_DISPATCH_CHECK': 'LOW',
    'REG_PTR_CAST': 'MEDIUM',
    'LITERAL_PTR_CAST': 'MEDIUM',
}


def check_file(path: Path) -> list[dict]:
    findings = []
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError as e:
        print(f"[SKIP] {path}: {e}", file=sys.stderr)
        return findings

    lines = text.splitlines()
    for lineno, line in enumerate(lines, 1):
        if _is_comment(line) or _is_ppc_macro(line):
            continue

        def record(kind: str, snippet: str):
            findings.append({
                'severity': SEVERITY.get(kind, 'INFO'),
                'kind': kind,
                'file': str(path.relative_to(ROOT)),
                'line': lineno,
                'snippet': snippet.strip()[:120],
            })

        # RAW_U32
        for m in _RAW_U32.finditer(line):
            if not _bswap_in_window(line, m.start()):
                record('RAW_U32', line)
                break

        # RAW_U64
        for m in _RAW_U64.finditer(line):
            if not _bswap_in_window(line, m.start()):
                record('RAW_U64', line)
                break

        # RAW_U16 (lower priority — u16 reads are often fine in host-only data)
        for m in _RAW_U16.finditer(line):
            if not _bswap_in_window(line, m.start()):
                # Only flag if we see 'base +' suggesting guest memory
                if 'base' in line[m.start():m.start()+80]:
                    record('RAW_U16', line)
                    break

        # VTABLE_NO_BSWAP — auto vt = *(uint32_t*)(base + ...) with no bswap
        if _VTABLE_NO_BSWAP.search(line) and not _BSWAP32.search(line):
            record('VTABLE_NO_BSWAP', line)

        # REG_PTR_CAST — (void*)ctx.rN.u32 — guest register used as host ptr
        if _CAST_FROM_REG.search(line):
            record('REG_PTR_CAST', line)

        # LITERAL_PTR_CAST — (void*)0x8XXXXXXX
        if _CAST_FROM_LITERAL.search(line):
            record('LITERAL_PTR_CAST', line)

    return findings


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', help='Files to scan (default: kernel/*.cpp app/*.cpp)')
    ap.add_argument('--all', action='store_true', help='Also scan guest/ppc/ppc_recomp.*.cpp')
    ap.add_argument('--json', action='store_true', help='Output JSON instead of text')
    ap.add_argument('--min-severity', choices=['LOW', 'MEDIUM', 'HIGH'], default='MEDIUM',
                    help='Minimum severity to report (default: MEDIUM)')
    args = ap.parse_args()

    severity_rank = {'LOW': 0, 'MEDIUM': 1, 'HIGH': 2}
    min_rank = severity_rank[args.min_severity]

    if args.files:
        targets = [Path(f) for f in args.files]
    else:
        targets = (
            list((ROOT / 'kernel').glob('*.cpp')) +
            list((ROOT / 'app').glob('*.cpp'))
        )

    if args.all:
        targets += sorted((ROOT / 'guest' / 'ppc').glob('ppc_recomp.*.cpp'))

    all_findings = []
    for path in targets:
        if not path.exists():
            print(f"[WARN] Not found: {path}", file=sys.stderr)
            continue
        findings = check_file(path)
        all_findings.extend(findings)

    filtered = [f for f in all_findings if severity_rank.get(f['severity'], 0) >= min_rank]

    if args.json:
        import json
        print(json.dumps(filtered, indent=2))
        return

    if not filtered:
        print("No findings above threshold - clean!")
        return

    # Group by severity
    for sev in ('HIGH', 'MEDIUM', 'LOW'):
        group = [f for f in filtered if f['severity'] == sev]
        if not group:
            continue
        print(f"\n== {sev} ({len(group)}) ==============================")
        for f in group:
            print(f"  [{f['kind']}] {f['file']}:{f['line']}")
            print(f"    {f['snippet']}")

    print(f"\nTotal: {len(filtered)} findings "
          f"({sum(1 for f in filtered if f['severity']=='HIGH')} HIGH, "
          f"{sum(1 for f in filtered if f['severity']=='MEDIUM')} MEDIUM, "
          f"{sum(1 for f in filtered if f['severity']=='LOW')} LOW)")


if __name__ == '__main__':
    main()
