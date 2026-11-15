#!/usr/bin/env python3
"""Summarize an XENTRC01 trace from the v2 oracle build (tools/xenia-trace/README.md).

    python tools/xenia-trace/summarize_trace.py <trace> [--vas tools/xenia-trace/burndown_vas.txt] [--json out.json]

Tag 0 (function entry): per VA the hit count, distinct callers (lr), threads, first r3.
With --vas, every listed VA is printed, including the ones Xenia NEVER entered (the
answer for "is this stub / special case ever reached on the console-faithful emulator").
Tag 2 (indirect call from an ICALL_FUNCS function): per call site (lr) the distinct
targets and counts. Tag 3 (target outside [0x820F0000,0x8290F5B8) or misaligned): every
distinct (lr, target) - the exact condition our PPC_CALL_INDIRECT_FUNC calls out-of-range.
Streams the file, so multi-GB traces are fine.
"""
import argparse
import collections
import json
import re
import struct
import sys

REC = struct.Struct("<IIQQII")


def load_vas(path):
    vas = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0]
        for tok in re.findall(r"(?:0[xX])?[0-9A-Fa-f]{6,8}", line):
            vas.append(int(tok, 16))
    return vas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--vas", help="VA list file (XENIA_GUEST_TRACE_VAS format)")
    ap.add_argument("--json", help="write the summary as JSON here")
    a = ap.parse_args()

    entry = collections.defaultdict(lambda: {"n": 0, "lr": collections.Counter(), "tid": set(), "r3": None})
    icall = collections.defaultdict(collections.Counter)   # lr -> target -> n
    bad = collections.Counter()                             # (lr, target) -> n
    total = 0
    with open(a.trace, "rb") as f:
        hdr = f.read(32)
        if len(hdr) < 32 or hdr[:8] != b"XENTRC01":
            sys.exit("not an XENTRC01 trace: %s" % a.trace)
        while True:
            buf = f.read(REC.size * 4096)
            if not buf:
                break
            for off in range(0, len(buf) - len(buf) % REC.size, REC.size):
                pc, lr, r3, r1, tid, tag = REC.unpack_from(buf, off)
                total += 1
                if tag == 0:
                    e = entry[pc]
                    e["n"] += 1
                    e["lr"][lr] += 1
                    e["tid"].add(tid)
                    if e["r3"] is None:
                        e["r3"] = r3 & 0xFFFFFFFF
                elif tag == 2:
                    icall[lr][pc] += 1
                elif tag == 3:
                    bad[(lr, pc)] += 1
                    icall[lr][pc] += 1

    out = {"records": total, "entries": {}, "icalls": {}, "bad": []}
    print("records: %d" % total)
    listed = load_vas(a.vas) if a.vas else sorted(entry)
    print("\n== function entries (tag 0)%s" % (" - every listed VA" if a.vas else ""))
    for va in listed:
        e = entry.get(va)
        if not e:
            print("  %08X  NEVER ENTERED" % va)
            out["entries"]["%08X" % va] = 0
            continue
        lrs = ", ".join("%08X x%d" % kv for kv in e["lr"].most_common(4))
        print("  %08X  n=%-9d threads=%d first_r3=%08X callers: %s" % (va, e["n"], len(e["tid"]), e["r3"], lrs))
        out["entries"]["%08X" % va] = {"n": e["n"], "threads": len(e["tid"]),
                                        "callers": {"%08X" % k: v for k, v in e["lr"].items()}}
    if icall:
        print("\n== indirect calls by call site (tag 2/3)")
        for lr in sorted(icall):
            tg = icall[lr]
            print("  lr %08X: %d calls, %d distinct targets: %s" % (
                lr, sum(tg.values()), len(tg), ", ".join("%08X x%d" % kv for kv in tg.most_common(6))))
            out["icalls"]["%08X" % lr] = {"%08X" % k: v for k, v in tg.items()}
    print("\n== OUT-OF-RANGE indirect calls (tag 3): %d distinct" % len(bad))
    for (lr, pc), n in bad.most_common():
        print("  lr %08X -> target %08X  x%d" % (lr, pc, n))
        out["bad"].append({"lr": "%08X" % lr, "target": "%08X" % pc, "n": n})
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
