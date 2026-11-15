#!/usr/bin/env python3
"""
Static recomp auditor: scans all ppc_recomp.*.cpp files for known-bad patterns
that indicate recompilation bugs. Produces a prioritized report.

Run:
    python scripts/audit_recomp.py
    python scripts/audit_recomp.py --file guest/ppc/ppc_recomp.61.cpp
    python scripts/audit_recomp.py --severity HIGH
    python scripts/audit_recomp.py --json > audit.json
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).parent.parent
RECOMP_DIR = ROOT / "guest" / "ppc"

# ── Pattern definitions ───────────────────────────────────────────────────────

@dataclass
class Finding:
    severity: str       # HIGH / MEDIUM / LOW
    category: str
    file: str
    line: int
    snippet: str
    description: str


# Each rule is (severity, category, description, compiled_regex_or_callable)
# The regex must match a single line (or we use multi-line context via callable).

SINGLE_LINE_RULES = [
    # ── Endianness bugs ───────────────────────────────────────────────────────
    (
        "HIGH", "ENDIAN",
        "uint32_t read from guest memory without bswap32 - likely big-endian value read as little-endian",
        re.compile(
            r'\*\s*\(\s*(?:const\s+)?(?:uint32_t|int32_t)\s*\*\s*\)'
            r'\s*\(\s*(?:getGuestBase\(\)|g_debugGuestBase|\bbase\b).*?\)'
            r'(?!\s*[\)]*\s*\)\s*;|\s*\+)'
        )
    ),
    (
        "HIGH", "ENDIAN",
        "PPC_LOAD_U32 result used without bswap32 in a pointer dereference context",
        re.compile(
            r'PPC_LOAD_U32\s*\([^)]+\)\s*\+\s*(?!0x[0-9a-fA-F]+\b)'
        )
    ),

    # ── Missing guest base offset ─────────────────────────────────────────────
    (
        "HIGH", "POINTER",
        "Guest VA cast directly to pointer without adding guest base - use getGuestBase()+va",
        re.compile(
            r'\(\s*(?:void|uint8_t|char)\s*\*\s*\)\s*(?:ctx\.[a-z]\d+\.u32|PPC_LOAD_U32\([^)]+\))'
            r'(?!\s*\+\s*(?:getGuestBase|g_debugGuestBase))'
        )
    ),

    # ── Vtable dispatch bugs ──────────────────────────────────────────────────
    (
        "HIGH", "VTABLE",
        "bctrl (indirect call) target loaded without bswap32 - vtable pointer is big-endian",
        re.compile(
            r'ctx\.ctr\.u64\s*=\s*\*\s*\(\s*(?:const\s+)?(?:uint32_t|uint64_t)\s*\*\s*\)'
            r'(?!\s*[\)]*.*bswap)'
        )
    ),

    # ── Null check missing ────────────────────────────────────────────────────
    (
        "MEDIUM", "NULL_CHECK",
        "PPC_LOAD_U32 result used as pointer without null check before dereference",
        re.compile(
            r'PPC_LOAD_U32\([^)]+\).*?;\s*\n'
            r'(?!.*?(?:if\s*\(|==\s*0|!=\s*0|nullptr|null))'
        )
    ),

    # ── Signed/unsigned cast issues ───────────────────────────────────────────
    (
        "MEDIUM", "SIGN",
        "Signed right shift on value loaded from guest - may propagate wrong sign bit",
        re.compile(
            r'(?:ctx\.[a-z]\d+\.s32|ctx\.[a-z]\d+\.s64)\s*>>\s*\d+'
        )
    ),
    (
        "LOW", "SIGN",
        "int32_t comparison against uint32_t constant - sign mismatch",
        re.compile(
            r'ctx\.[a-z]\d+\.s32\s*(?:==|!=|<|>|<=|>=)\s*0x[0-9A-Fa-f]{5,}'
        )
    ),

    # ── PPC_STORE_U32 patterns that might be wrong ───────────────────────────
    (
        "MEDIUM", "STORE",
        "PPC_STORE_U32 storing a host pointer (non-bswapped) into guest memory",
        re.compile(
            r'PPC_STORE_U32\s*\([^,]+,\s*(?:reinterpret_cast|static_cast)\s*<\s*(?:uint32_t|int32_t)\s*>'
        )
    ),

    # ── Stub returns wrong type ───────────────────────────────────────────────
    (
        "LOW", "STUB",
        "stub_generic_success used - verify this XDK function actually returns 0 on success",
        re.compile(r'\bstub_generic_success\b')
    ),
    (
        "LOW", "STUB",
        "ctx.r3 not set before return - function may return garbage to caller",
        re.compile(r'return;\s*$')  # bare return in a void PPC func - might be ok, flag for review
    ),
]

# Patterns that need 2-line context (line N and line N+1)
TWO_LINE_RULES = [
    (
        "HIGH", "ENDIAN",
        "uint32_t read immediately used as pointer without bswap - classic vtable load bug",
        re.compile(r'ctx\.[a-z]\d+\.u32\s*=\s*PPC_LOAD_U32'),
        re.compile(r'getGuestBase\(\)\s*\+\s*ctx\.[a-z]\d+\.u32(?!\s*\)\s*;)'),
    ),
]

# ── Scanner ───────────────────────────────────────────────────────────────────

def scan_file(path: Path) -> list[Finding]:
    findings = []
    rel = path.relative_to(ROOT).as_posix()

    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    for i, line in enumerate(lines, 1):
        stripped = line.rstrip()

        for sev, cat, desc, pattern in SINGLE_LINE_RULES:
            if pattern.search(stripped):
                findings.append(Finding(
                    severity=sev, category=cat, file=rel, line=i,
                    snippet=stripped[:120], description=desc
                ))

    # Two-line context rules
    for i in range(len(lines) - 1):
        line_a = lines[i].rstrip()
        line_b = lines[i + 1].rstrip()
        for sev, cat, desc, pat_a, pat_b in TWO_LINE_RULES:
            if pat_a.search(line_a) and pat_b.search(line_b):
                findings.append(Finding(
                    severity=sev, category=cat, file=rel, line=i + 1,
                    snippet=f"{line_a[:80]} | {line_b[:80]}", description=desc
                ))

    return findings


def priority_key(f: Finding):
    return {"HIGH": 0, "MEDIUM": 1, "LOW": 2}.get(f.severity, 3)


def run_audit(target_file: Path | None = None, min_severity: str = "LOW") -> list[Finding]:
    sev_order = ["HIGH", "MEDIUM", "LOW"]
    min_idx = sev_order.index(min_severity) if min_severity in sev_order else 2

    if target_file:
        files = [target_file]
    else:
        files = sorted(RECOMP_DIR.glob("ppc_recomp.*.cpp"))

    all_findings = []
    for f in files:
        found = scan_file(f)
        kept = [x for x in found if sev_order.index(x.severity) <= min_idx]
        all_findings.extend(kept)
        if kept:
            print(f"  {f.name}: {len(kept)} findings", file=sys.stderr)

    all_findings.sort(key=priority_key)
    return all_findings


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Static recomp bug pattern auditor")
    parser.add_argument("--file", help="Scan only this file")
    parser.add_argument("--severity", default="LOW", choices=["HIGH", "MEDIUM", "LOW"],
                        help="Minimum severity to report")
    parser.add_argument("--json", action="store_true", help="Output JSON")
    parser.add_argument("--summary", action="store_true", help="Summary counts only")
    args = parser.parse_args()

    target = Path(args.file) if args.file else None
    findings = run_audit(target, args.severity)

    if args.json:
        print(json.dumps([
            {"severity": f.severity, "category": f.category, "file": f.file,
             "line": f.line, "snippet": f.snippet, "description": f.description}
            for f in findings
        ], indent=2))
    elif args.summary:
        by_sev = {}
        by_cat = {}
        for f in findings:
            by_sev[f.severity] = by_sev.get(f.severity, 0) + 1
            by_cat[f.category] = by_cat.get(f.category, 0) + 1
        print(f"Total: {len(findings)} findings")
        for k, v in sorted(by_sev.items()):
            print(f"  {k}: {v}")
        print("By category:")
        for k, v in sorted(by_cat.items(), key=lambda x: -x[1]):
            print(f"  {k}: {v}")
    else:
        for f in findings:
            print(f"[{f.severity}] {f.category}  {f.file}:{f.line}")
            print(f"  {f.description}")
            print(f"  > {f.snippet}")
            print()
        print(f"Total: {len(findings)} findings")
