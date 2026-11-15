#!/usr/bin/env python3
"""test_interp_vs_qemu.py - validate the clean-room oracle (xenon_interp.py) against a THIRD, fully
independent PowerPC implementation: QEMU's TCG PPC target via Unicorn (pip install unicorn), per
instruction, on random operands.

Unicorn's PPC64 target cannot have MSR[FP] enabled from the API (writes are ignored), so this uses
the 32-bit target (UC_MODE_PPC32, a 7xx/74xx-class CPU): it covers every 32-bit integer form the
Havok query path uses (results compared on the low 32 bits, plus CR and XER[CA]) and the scalar
FPU in IEEE hardware semantics (compared against the interpreter's mode="isa": exact single
rounding for fadds/fmuls/fdivs/fmadds/fmsubs/fnmsubs, fsel, frsp, fcmpu, fctiwz).

    python test_interp_vs_qemu.py [--n 400] [--seed 1]
Exit 0 = every sampled instruction matched. (AltiVec is not checked here: Unicorn 2.1.4 faults
inside the library on lvx with MSR[VEC] set; the VMX/VMX128 semantics follow Xenia's
ppc_emit_altivec.cc / x64 sequences, the project's declared oracle.)
"""
import argparse
import math
import random
import struct
import sys
import os

from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import *  # noqa: F401,F403
import unicorn.ppc_const as P

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xenon_interp as X

CODE = 0x10000
M32 = 0xFFFFFFFF


def enc_d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def enc_x(rt, ra, rb, xo, rc=0, op=31):
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def enc_xo(rt, ra, rb, xo, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def enc_m(op, rs, ra, sh, mb, me, rc=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


def enc_a(op, frt, fra, frb, frc, xo, rc=0):
    return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | rc


def int_templates(rng):
    """(name, word) - dest r5, sources r3/r4."""
    t = []
    si = rng.randrange(-0x8000, 0x8000)
    ui = rng.randrange(0, 0x10000)
    sh, mb, me = rng.randrange(32), rng.randrange(32), rng.randrange(32)
    t += [("addi", enc_d(14, 5, 3, si)), ("addis", enc_d(15, 5, 3, si)), ("addic", enc_d(12, 5, 3, si)),
          ("addic.", enc_d(13, 5, 3, si)), ("subfic", enc_d(8, 5, 3, si)), ("mulli", enc_d(7, 5, 3, si)),
          ("ori", enc_d(24, 3, 5, ui)), ("oris", enc_d(25, 3, 5, ui)), ("xori", enc_d(26, 3, 5, ui)),
          ("andi.", enc_d(28, 3, 5, ui)), ("andis.", enc_d(29, 3, 5, ui)),
          ("cmpwi", enc_d(11, 6 << 2, 3, si)), ("cmplwi", enc_d(10, 6 << 2, 3, ui)),
          ("rlwinm", enc_m(21, 3, 5, sh, mb, me)), ("rlwinm.", enc_m(21, 3, 5, sh, mb, me, 1)),
          ("rlwimi", enc_m(20, 3, 5, sh, mb, me)), ("rlwnm", enc_m(23, 3, 5, 4, mb, me)),
          ("add", enc_xo(5, 3, 4, 266)), ("add.", enc_xo(5, 3, 4, 266, 1)), ("subf", enc_xo(5, 3, 4, 40)),
          ("subf.", enc_xo(5, 3, 4, 40, 1)), ("subfc", enc_xo(5, 3, 4, 8)), ("subfe", enc_xo(5, 3, 4, 136)),
          ("addc", enc_xo(5, 3, 4, 10)), ("adde", enc_xo(5, 3, 4, 138)), ("addze", enc_xo(5, 3, 0, 202)),
          ("neg", enc_xo(5, 3, 0, 104)), ("mullw", enc_xo(5, 3, 4, 235)), ("mulhw", enc_xo(5, 3, 4, 75)),
          ("mulhwu", enc_xo(5, 3, 4, 11)), ("divw", enc_xo(5, 3, 4, 491)), ("divwu", enc_xo(5, 3, 4, 459)),
          ("and", enc_x(3, 5, 4, 28)), ("and.", enc_x(3, 5, 4, 28, 1)), ("andc", enc_x(3, 5, 4, 60)),
          ("or", enc_x(3, 5, 4, 444)), ("xor", enc_x(3, 5, 4, 316)), ("xor.", enc_x(3, 5, 4, 316, 1)),
          ("nor", enc_x(3, 5, 4, 124)), ("cntlzw", enc_x(3, 5, 0, 26)), ("extsb", enc_x(3, 5, 0, 954)),
          ("extsb.", enc_x(3, 5, 0, 954, 1)), ("extsh", enc_x(3, 5, 0, 922)), ("extsh.", enc_x(3, 5, 0, 922, 1)),
          ("slw", enc_x(3, 5, 4, 24)), ("srw", enc_x(3, 5, 4, 536)), ("sraw", enc_x(3, 5, 4, 792)),
          ("srawi", enc_x(3, 5, sh, 824)), ("cmpw", enc_x(6 << 2, 3, 4, 0)), ("cmplw", enc_x(6 << 2, 3, 4, 32))]
    return t


def fp_templates():
    """(name, word) - dest f5, sources f1/f2/f3."""
    return [("fadds", enc_a(59, 5, 1, 2, 0, 21)), ("fsubs", enc_a(59, 5, 1, 2, 0, 20)),
            ("fmuls", enc_a(59, 5, 1, 0, 3, 25)), ("fdivs", enc_a(59, 5, 1, 2, 0, 18)),
            ("fmadds", enc_a(59, 5, 1, 2, 3, 29)), ("fmsubs", enc_a(59, 5, 1, 2, 3, 28)),
            ("fnmsubs", enc_a(59, 5, 1, 2, 3, 30)), ("fsel", enc_a(63, 5, 1, 2, 3, 23)),
            ("frsp", enc_x(5, 0, 1, 12, 0, 63)), ("fcmpu", enc_x(6 << 2, 1, 2, 0, 0, 63)),
            ("fctiwz", enc_x(5, 0, 1, 15, 0, 63)), ("fabs", enc_x(5, 0, 1, 264, 0, 63)),
            ("fneg", enc_x(5, 0, 1, 40, 0, 63))]


def rnd_int(rng):
    k = rng.random()
    if k < 0.2:
        return rng.choice([0, 1, M32, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x8000, 31, 32])
    return rng.randrange(0, 1 << 32)


def rnd_single(rng):
    k = rng.random()
    if k < 0.1:
        return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 3.0e38, -1.17549435e-38, 1e-45])
    return struct.unpack(">f", struct.pack(">f", rng.uniform(-1, 1) * 10 ** rng.uniform(-6, 6)))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(CODE, 0x1000)
    uc.reg_write(P.UC_PPC_REG_MSR, 0x2000)    # FP available (the 32-bit target honours this)
    mem = X.Memory()
    cpu = X.CPU(mem, "xenia")
    fails = 0
    total = 0
    names = set()
    gpr = [getattr(P, "UC_PPC_REG_%d" % i) for i in range(32)]
    fpr = [getattr(P, "UC_PPC_REG_FPR%d" % i) for i in range(32)]
    for it in range(a.n):
        for name, w in int_templates(rng):
            v3, v4, v5 = rnd_int(rng), rnd_int(rng), rnd_int(rng)
            if name in ("sraw", "slw", "srw") and rng.random() < 0.5:
                v4 = rng.randrange(64)
            ca = rng.randrange(2)
            # Unicorn's XER accessor does not carry QEMU's separate CA flag, so CA goes in and
            # out through instructions: addic r0,r6,-1 sets CA = (r6 != 0); mfxer r7 reads it back.
            uc.mem_write(CODE, struct.pack(">IIII", 0x3006FFFF, w, 0x7CE102A6, 0x60000000))
            uc.reg_write(gpr[3], v3); uc.reg_write(gpr[4], v4); uc.reg_write(gpr[5], v5)
            uc.reg_write(gpr[6], ca)
            uc.reg_write(P.UC_PPC_REG_CR, 0)
            uc.emu_start(CODE, CODE + 12, count=3)
            q5 = uc.reg_read(gpr[5]) & M32
            qcr = uc.reg_read(P.UC_PPC_REG_CR) & M32
            qca = (uc.reg_read(gpr[7]) >> 29) & 1
            # interpreter: operands zero-extended 32-bit values (the 32-bit view)
            cpu.gpr = [0] * 32
            cpu.gpr[3], cpu.gpr[4], cpu.gpr[5] = X.sx32(v3) & X.M64, X.sx32(v4) & X.M64, X.sx32(v5) & X.M64
            cpu.cr = [0] * 8; cpu.ca = ca; cpu.so = 0
            fn = cpu.decode(0x1000, w)
            fn(cpu)
            i5 = cpu.gpr[5] & M32
            icr = 0
            for f in range(8):
                icr = (icr << 4) | cpu.cr[f]
            ok = (q5 == i5) and (qcr == icr)
            if name in ("subfc", "subfe", "addc", "adde", "addze", "addic", "addic.", "subfic", "sraw", "srawi"):
                ok = ok and (qca == cpu.ca)
            if name in ("divw", "divwu") and (v4 == 0 or (name == "divw" and v3 == 0x80000000 and v4 == M32)):
                ok = True   # undefined result
            total += 1
            names.add(name)
            if not ok:
                fails += 1
                if fails <= 12:
                    print("INT MISMATCH %-8s w=%08X r3=%08X r4=%08X r5=%08X ca=%d : qemu r5=%08X cr=%08X ca=%d | interp r5=%08X cr=%08X ca=%d"
                          % (name, w, v3, v4, v5, ca, q5, qcr, qca, i5, icr, cpu.ca))
        for name, w in fp_templates():
            f1, f2, f3 = rnd_single(rng), rnd_single(rng), rnd_single(rng)
            uc.mem_write(CODE, struct.pack(">II", w, 0x60000000))
            for r, v in ((1, f1), (2, f2), (3, f3), (5, 0.0)):
                uc.reg_write(fpr[r], X.d_bits(v))
            uc.reg_write(P.UC_PPC_REG_CR, 0)
            uc.reg_write(P.UC_PPC_REG_FPSCR, 0)
            uc.emu_start(CODE, CODE + 4, count=1)
            qf = uc.reg_read(fpr[5]) & X.M64
            qcr = uc.reg_read(P.UC_PPC_REG_CR) & M32
            cpu.mode = "isa"
            cpu.fpr = [0.0] * 32
            cpu.fpr[1], cpu.fpr[2], cpu.fpr[3] = f1, f2, f3
            cpu.cr = [0] * 8
            cpu.decode(0x1000, w)(cpu)
            cpu.mode = "xenia"
            if_ = X.d_bits(cpu.fpr[5])
            icr = 0
            for f in range(8):
                icr = (icr << 4) | cpu.cr[f]
            if name == "fcmpu":
                ok = (qcr >> 4) & 0xF == cpu.cr[6] & 0xF or ((qcr >> 4) & 0xE) == (cpu.cr[6] & 0xE)
            elif name == "fctiwz":
                # QEMU returns 0x80000000 for positive overflow; the ISA (and the interpreter)
                # saturate to 0x7FFFFFFF - compare in range only
                ok = (qf & M32) == (if_ & M32) or abs(f1) >= 2147483648.0
            else:
                qd, idd = X.bits_d(qf), X.bits_d(if_)
                ok = qf == if_ or (math.isnan(qd) and math.isnan(idd))
            total += 1
            names.add(name)
            if not ok:
                fails += 1
                if fails <= 24:
                    print("FP MISMATCH %-8s f1=%r f2=%r f3=%r : qemu %016X (%r) cr=%08X | interp %016X (%r) cr6=%X"
                          % (name, f1, f2, f3, qf, X.bits_d(qf), qcr, if_, X.bits_d(if_), cpu.cr[6]))
    print("interp vs QEMU: %d instruction executions over %d forms, %d mismatches" % (total, len(names), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

