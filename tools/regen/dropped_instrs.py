#!/usr/bin/env python3
"""Count instruction comments with NO emitted implementation line following.

An emission looks like:
    // <mnemonic> <operands>
    <one or more code lines>
A dropped instruction is a `// instr` line followed directly by another
`// instr` line, a label, or a closing brace.
"""
import os
import re
import sys
from collections import Counter

INSTR = re.compile(r"^\t// ([a-z][a-z0-9_.+-]*)")
CODE = re.compile(r"^\t[^/]")


def scan(d):
    dropped = Counter()
    examples = {}
    for name in sorted(os.listdir(d)):
        if not (name.startswith("ppc_recomp.") and name.endswith(".cpp")):
            continue
        lines = open(os.path.join(d, name), encoding="utf-8", errors="replace").read().splitlines()
        for i, ln in enumerate(lines):
            m = INSTR.match(ln)
            if not m:
                continue
            mnem = m.group(1)
            nxt = lines[i + 1] if i + 1 < len(lines) else "}"
            if INSTR.match(nxt) or nxt.startswith("loc_") or nxt == "}":
                dropped[mnem] += 1
                examples.setdefault(mnem, f"{name}:{i+1}")
    return dropped, examples


def main():
    for label, d in [("OLD", sys.argv[1]), ("NEW", sys.argv[2])]:
        dropped, ex = scan(d)
        print(f"== {label} ({d}) — dropped-instruction comment sites ==")
        for mnem, n in dropped.most_common(40):
            print(f"  {mnem:16s} {n:6d}   e.g. {ex[mnem]}")
        print(f"  TOTAL: {sum(dropped.values())}")


if __name__ == "__main__":
    main()
