#!/usr/bin/env python3
"""run_script.py - ORACLE side of the golden-call differential: executes a golden_call_harness
script with the clean-room interpreter (xenon_interp.py) on the same golden memory blob and prints
the same text the recomp harness prints.

    python run_script.py blob script out [--mode xenia|recomp] [--cov cov.json] [--jobs N]

--jobs N splits the script at CASE boundaries across N worker processes (cases are independent:
END restores memory on both sides)."""
import argparse
import json
import os
import struct
import sys
import time
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xenon_interp as X

SCRATCH_LO, SCRATCH_HI = 0x70000000, 0x71000000
STACK_TOP = 0x70F00000
PCR = 0x70E00000
VA_COLLECT = 0x8290F500
VA_TRAP = 0x8290F504
IMPORTS = {
    0x828EE16C: "KeTlsGetValue",
    0x828EE49C: "KeTlsSetValue",
    0x828EE17C: "RtlEnterCriticalSection",
    0x828EE18C: "RtlLeaveCriticalSection",
    0x828EEA2C: "RtlTryEnterCriticalSection",
    0x828EEBCC: "NtDuplicateObject",
    0x828EE4FC: "NtClose",
}


MUTATIONS = []


def make_cpu(blob, mode, mutations=()):
    mem = X.Memory()
    mem.load_blob(blob)
    for m in mutations:
        va, hx = m.split(":")
        mem.write_bytes(int(va, 16), bytes.fromhex(hx), track=False)
    mem.snapshot()
    cpu = X.CPU(mem, mode)
    cpu.out_log = []
    cpu.tls = {}

    def chain(body):
        parts = []
        d = 0
        while body and d < 8:
            parts.append("%08X,%08X" % (mem.r32(body), mem.r32(body + 4)))
            body = mem.r32(body + 12)
            d += 1
        return "/".join(parts)

    def collect(c):
        self, pt = c.gpr[3] & 0xFFFFFFFF, c.gpr[4] & 0xFFFFFFFF
        w = [mem.r32(pt + 4 * i) for i in range(8)]
        c.out_log.append("P c=%08X pos=%08X,%08X,%08X,%08X nd=%08X,%08X,%08X,%08X A=%s B=%s" % (
            self, *w, chain(mem.r32(pt + 32)), chain(mem.r32(pt + 36))))
        mem.w32(self + 12, mem.r32(self + 12) + 1)
        if mem.r32(self + 8) == 1:
            d = X.bits_f32(w[7]); e = X.bits_f32(mem.r32(self + 4))
            if d < e:
                mem.w32(self + 4, w[7])

    cpu.hooks[VA_COLLECT] = collect
    for i in range(8):
        if i == 1:
            continue
        def trap(c, i=i):
            c.out_log.append("VCALL %d" % i)
            c.gpr[3] = 0
        cpu.hooks[VA_TRAP + 4 * i] = trap

    def imp(name):
        def h(c):
            c.log.append("import %s r3=%X" % (name, c.gpr[3]))
            if name == "KeTlsGetValue":
                c.gpr[3] = c.tls.get(c.gpr[3] & 0xFFFFFFFF, 0)
            elif name == "KeTlsSetValue":
                c.tls[c.gpr[3] & 0xFFFFFFFF] = c.gpr[4] & 0xFFFFFFFF
                c.gpr[3] = 1
            elif name in ("RtlEnterCriticalSection", "RtlLeaveCriticalSection"):
                c.cs_calls = getattr(c, "cs_calls", 0) + 1
                c.gpr[3] = 0
            elif name == "NtDuplicateObject":
                if c.gpr[4] & 0xFFFFFFFF:
                    c.mem.w32(c.gpr[4] & 0xFFFFFFFF, c.gpr[3] & 0xFFFFFFFF)
                c.out_log.append("IMPORT NtDuplicateObject")
                c.gpr[3] = 0
            elif name == "NtClose":
                c.out_log.append("IMPORT NtClose")
                c.gpr[3] = 0
            elif name == "RtlTryEnterCriticalSection":
                c.cs_calls = getattr(c, "cs_calls", 0) + 1
                c.gpr[3] = 1
            else:
                c.out_log.append("IMPORT-UNEXPECTED")
                c.gpr[3] = 0
        return h
    for va, name in IMPORTS.items():
        cpu.hooks[va] = imp(name)
    return cpu


# HEAP <lo> <hi> (iter-467): the guest heap entry points hand out blocks from a bump arena in
# scratch, exactly as golden_call_harness.cpp does (the golden dump holds no usable process heap).
BUMP_HEAP_VAS = (0x8246CA80, 0x8246D368, 0x82468008, 0x824680A8)


def install_bump_heap(cpu, lo, hi):
    st = {"cur": lo, "hi": hi}

    def bump(size, align, zero):
        align = max(align, 16)
        p = (st["cur"] + align - 1) & ~(align - 1)
        if p > st["hi"] or size > st["hi"] - p:
            return 0
        st["cur"] = p + ((size + 15) & ~15)
        if zero:
            cpu.mem.write_bytes(p, bytes(size))
        return p

    def alloc(c):     # RtlAllocateHeap(heap, flags, size)
        c.gpr[3] = bump(c.gpr[5] & 0xFFFFFFFF, 16, bool(c.gpr[4] & 8))

    def free(c):      # RtlFreeHeap(heap, flags, block)
        c.gpr[3] = 1

    def palloc(c):    # XMemAllocPhysical(size, address, alignment, protect)
        al = c.gpr[5] & 0xFFFFFFFF
        c.gpr[3] = bump(c.gpr[3] & 0xFFFFFFFF, al if 0 < al <= 0x10000 else 16, False)

    def pfree(c):     # XMemFreePhysical(block)
        c.gpr[3] = 0
    for va, h in zip(BUMP_HEAP_VAS, (alloc, free, palloc, pfree)):
        cpu.hooks[va] = h


def remove_bump_heap(cpu):
    for va in BUMP_HEAP_VAS:
        cpu.hooks.pop(va, None)


def end_case(cpu, out):
    mem = cpu.mem
    changed = []
    for pn in sorted(mem.dirty):
        old = mem.pristine.get(pn)
        cur = mem.pages.get(pn)
        for k in range(16):
            va = (pn << 16) | (k << 12)
            if SCRATCH_LO <= va < SCRATCH_HI:
                continue
            a = cur[k << 12:(k + 1) << 12] if cur is not None else bytes(4096)
            b = old[k << 12:(k + 1) << 12] if old is not None else bytes(4096)
            if a != b:
                h = 1469598103934665603
                for byte in a:
                    h ^= byte
                    h = (h * 1099511628211) & ((1 << 64) - 1)
                changed.append("CHANGED %08X %016X" % (va, h))
    for l in changed:
        out.append(l)
    mem.restore()
    cpu.out_log = []
    cpu.tls = {}


def run_lines(cpu, lines, stats_out=None):
    out = []
    t0 = time.time()
    case = None
    case_steps = {}
    for raw in lines:
        p = raw.rstrip("\r\n")
        if not p or p.startswith("#"):
            continue
        cmd = p.split()[0]
        if cmd == "CASE":
            out.append(p)
            case = p[5:]
        elif cmd == "W":
            _, va, hx = p.split(None, 2)
            cpu.mem.write_bytes(int(va, 16), bytes.fromhex(hx))
        elif cmd == "CALL":
            parts = p.split()
            va = int(parts[1], 16)
            cpu.gpr = [0] * 32
            cpu.fpr = [0.0] * 32
            cpu.vr = [[0, 0, 0, 0] for _ in range(128)]
            cpu.cr = [0] * 8
            cpu.ca = cpu.so = 0
            cpu.ctr = 0
            cpu.fpscr = 0     # the harness starts every CALL with csr 0x1F80 (RN nearest)
            cpu.msr = 0       # and a zeroed PPCContext (msr 0)
            cpu.gpr[1] = STACK_TOP
            cpu.gpr[13] = PCR
            for a in parts[2:]:
                k, v = a.split("=")
                n = int(k[1:])
                if k[0] == "r":
                    cpu.gpr[n] = int(v, 16)
                elif k[0] == "v":   # vN=<32 hex>: the 16 guest (big-endian) bytes, element 0 first
                    cpu.vr[n] = list(struct.unpack(">4I", bytes.fromhex(v.rjust(32, "0"))))
                else:
                    cpu.fpr[n] = X.bits_d(int(v, 16))
            try:
                steps = cpu.call(va)
                case_steps[case] = case_steps.get(case, 0) + steps
            except (X.GuestFault, X.Unimplemented) as e:
                out.append("ORACLE-ERROR %s" % e)
                sys.stderr.write("ORACLE-ERROR case=%s %s lr=%08X r3=%08X r12=%08X ctr=%08X\n" % (case, e, cpu.lr & 0xFFFFFFFF, cpu.gpr[3] & 0xFFFFFFFF, cpu.gpr[12] & 0xFFFFFFFF, cpu.ctr & 0xFFFFFFFF))
                case_steps[case] = case_steps.get(case, 0)
        elif cmd == "CS":
            out.append("CS %d" % getattr(cpu, "cs_calls", 0))
            cpu.cs_calls = 0
        elif cmd == "HEAP":   # HEAP <lo> <hi>: the bump arena until END (golden_call_harness.cpp)
            _, lo, hi = p.split()
            install_bump_heap(cpu, int(lo, 16), int(hi, 16))
        elif cmd == "TLS":
            _, idx, val = p.split()
            cpu.tls[int(idx, 16)] = int(val, 16)
        elif cmd == "PR":
            n = int(p.split()[1][1:])
            out.append("r%d=%016X" % (n, cpu.gpr[n] & X.M64))
        elif cmd == "PV":
            n = int(p.split()[1][1:])
            out.append("v%d=%s" % (n, struct.pack(">4I", *cpu.vr[n]).hex().upper()))
        elif cmd == "PCR":   # PCR <field>: the 4 bits LT GT EQ SO of one CR field
            n = int(p.split()[1])
            out.append("cr%d=%X" % (n, cpu.cr[n] & 0xF))
        elif cmd == "PF":
            n = int(p.split()[1][1:])
            out.append("f%d=%016X" % (n, X.d_bits(cpu.fpr[n])))
        elif cmd == "D":
            _, va, ln = p.split()
            va, ln = int(va, 16), int(ln, 16)
            out.append("D %08X %s" % (va, cpu.mem.read_bytes(va, ln).hex().upper()))
        elif cmd == "DARR":
            _, va, es = p.split()
            va, es = int(va, 16), int(es, 16)
            data, size = cpu.mem.r32(va), cpu.mem.r32(va + 4)
            out.append("DARR %08X n=%d %s" % (va, size, cpu.mem.read_bytes(data, min(size, 65536) * es).hex().upper()))
        elif cmd == "LOG":
            out.extend(cpu.out_log)
            cpu.out_log = []
        elif cmd == "END":
            end_case(cpu, out)
            remove_bump_heap(cpu)
    if stats_out is not None:
        stats_out["steps"] = case_steps
        stats_out["secs"] = time.time() - t0
    return out


_G = {}


def _worker_init(blob, mode, mutations):
    _G["cpu"] = make_cpu(blob, mode, mutations)


def _worker(chunk):
    st = {}
    cpu = _G["cpu"]
    cpu.stats = {}
    cpu.branch_cov = {}
    out = run_lines(cpu, chunk, st)
    return out, st, cpu.stats, {("%08X" % k): v for k, v in cpu.branch_cov.items()}, cpu.cr0_width_mismatch, cpu.ca_width_mismatch, cpu.log[-50:], list(cpu.cache.keys())


def split_cases(lines, n):
    cases = []
    cur = []
    for l in lines:
        if l.startswith("CASE ") and cur:
            cases.append(cur)
            cur = []
        cur.append(l)
    if cur:
        cases.append(cur)
    k = max(1, (len(cases) + n * 4 - 1) // (n * 4))
    return [sum(cases[i:i + k], []) for i in range(0, len(cases), k)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blob")
    ap.add_argument("script")
    ap.add_argument("out")
    ap.add_argument("--mode", default="xenia")
    ap.add_argument("--cov")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--mutate", action="append", default=[], help="va:hexbytes written into the ORACLE's memory only")
    ap.add_argument("--watch", action="append", default=[],
                    help="dword VA (hex): report every guest store to it (pc, lr, value) on stderr; --jobs 1")
    a = ap.parse_args()
    lines = open(a.script).read().splitlines()
    t0 = time.time()
    agg_stats, agg_br, steps = {}, {}, {}
    cr0w = caw = 0
    notes = []
    allpcs = set()
    if a.jobs <= 1:
        cpu = make_cpu(a.blob, a.mode, a.mutate)
        if a.watch:
            cpu.mem.watch = {int(w, 16) for w in a.watch}
            cpu.mem.cpu = cpu
        st = {}
        out = run_lines(cpu, lines, st)
        if a.watch:
            for (w, pc, lr, sa, n, val, gpr) in cpu.mem.watch_hits:
                regs = " ".join("r%d=%08X" % (i, gpr[i] & 0xFFFFFFFF) for i in (1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 29, 30, 31)) if gpr else ""
                sys.stderr.write("WATCH %08X pc=%08X lr=%08X store=%08X+%d val=%08X %s\n" % (w, pc, lr, sa, n, val, regs))
        agg_stats, steps = cpu.stats, st["steps"]
        agg_br = {("%08X" % k): v for k, v in cpu.branch_cov.items()}
        cr0w, caw, notes = cpu.cr0_width_mismatch, cpu.ca_width_mismatch, cpu.log[-50:]
        allpcs = set(cpu.cache.keys())
    else:
        chunks = split_cases(lines, a.jobs)
        out = []
        with Pool(a.jobs, initializer=_worker_init, initargs=(a.blob, a.mode, a.mutate)) as pool:
            for o, st, s, br, c0, cw, lg, pcs in pool.imap(_worker, chunks):
                allpcs.update(pcs)
                out.extend(o)
                steps.update(st["steps"])
                for k, v in s.items():
                    agg_stats[k] = agg_stats.get(k, 0) + v
                for k, v in br.items():
                    t = agg_br.setdefault(k, [0, 0])
                    t[0] += v[0]; t[1] += v[1]
                cr0w += c0; caw += cw
                notes.extend(lg)
    with open(a.out, "w") as f:
        f.write("\n".join(out) + "\n")
    tot = sum(steps.values())
    print("oracle mode=%s cases=%d steps=%d secs=%.1f cr0_width_mismatch=%d ca_width_mismatch=%d" % (
        a.mode, len(steps), tot, time.time() - t0, cr0w, caw))
    if a.cov:
        json.dump({"mnemonics": agg_stats, "branches": agg_br, "steps": steps, "notes": notes[-200:],
                   "pcs": sorted(allpcs)},
                  open(a.cov, "w"), indent=0)


if __name__ == "__main__":
    main()
