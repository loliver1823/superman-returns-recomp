#!/usr/bin/env python3
"""xenon_interp.py - clean-room PowerPC (Xenon: PPC64 + VMX + VMX128) interpreter, the ORACLE
side of the golden-call differential (scripts/difftest/golden_call, docs/TOOLKIT.md 1.3b).

Independent of XenonRecomp's lowering: every instruction is decoded from the machine word in
the golden memory image and executed per the Power ISA (integer / branch / FPU) and per Xenia's
PPC frontend (reference/xenia-canary/src/xenia/cpu/ppc/ppc_emit_altivec.cc) for VMX / VMX128 -
Xenia is this project's declared oracle. It is NOT derived from guest/ppc/*.cpp.

Two arithmetic modes (the control flow / integer / memory semantics are identical in both):
  mode="xenia"  - the reference: fused vmaddfp/vnmsubfp (one rounding, Xenia MulAdd), Xenia's
                  dot product (double products, (p0+p2)+p1(+p3) in double, one rounding, overflow
                  -> QNaN), fmadds = MulAdd fused in double then ToSingle, scalar single ops =
                  double op then ToSingle, CR0 on the low 32 bits (Xenia UpdateCR truncates),
                  maxps|maxps OR for vmaxfp, RNE vrfin, stfs rounds.
  mode="recomp" - the arithmetic conventions XenonRecomp's lowering uses (unfused vmaddfp,
                  simde's portable dp_ps summation, fmadds unfused in double then float, 32-bit CA,
                  maxps/minps operand rules; vrfin = RNE since the iter-463 roundevenf fix,
                  SR_INTERP_PRE463_ROUNDEVEN=1 models an older pinned lib).
  mode="isa"    - exact Power ISA: single rounding of the exact result for single-precision ops,
                  64-bit CR0 compares, stfs without rounding. Running both modes attributes any divergence:
                  if recomp-mode matches our recomp bit-exactly and xenia-mode does not, the
                  difference is purely the named arithmetic convention; if recomp-mode ALSO
                  differs, the recomp mis-lowers something else (a real codegen bug).

VMX float ops run in non-Java mode (VSCR[NJ]=1 on Xenon): denormal inputs and outputs flush to
signed zero. Scalar FPU is IEEE (no flush), round-to-nearest-even.
"""
import math
import struct
from fractions import Fraction

import numpy as np

np.seterr(all="ignore")

# recomp mode, vrfin: guest libs built before iter-463 carried the roundevenf tie-direction bug
# (0.5 -> 2, 2.5 -> 4). Set SR_INTERP_PRE463_ROUNDEVEN=1 to model such a (pinned) lib.
PRE463_ROUNDEVEN = __import__("os").environ.get("SR_INTERP_PRE463_ROUNDEVEN", "0") == "1"

U8 = struct.Struct(">B"); U16 = struct.Struct(">H"); U32 = struct.Struct(">I"); U64 = struct.Struct(">Q")
F32 = struct.Struct(">f"); F64 = struct.Struct(">d")
M64 = (1 << 64) - 1
M32 = 0xFFFFFFFF
PAGE_BITS = 16
PAGE = 1 << PAGE_BITS
PMASK = PAGE - 1


class GuestFault(Exception):
    pass


class Unimplemented(Exception):
    pass


# ----------------------------------------------------------------------------------------------
# memory: sparse 64 KiB pages, big-endian accessors, dirty-page tracking for per-case restore
# ----------------------------------------------------------------------------------------------
class Memory:
    def __init__(self):
        self.pages = {}
        self.dirty = set()
        self.pristine = {}
        self.oob_reads = 0
        # iter-465 (codegen lane): write watch. watch = set of dword VAs; every guest store that
        # overlaps one records (va, pc, lr, store addr, size, new dword) in watch_hits - the
        # oracle names the writer of a divergent dword (run_script.py --watch).
        self.watch = None
        self.watch_hits = []
        self.cpu = None

    def _wh(self, a, n):
        for w in self.watch:
            if a < w + 4 and w < a + n:
                c = self.cpu
                self.watch_hits.append((w, c.pc if c else 0, c.lr if c else 0, a, n, self.r32(w),
                                        list(c.gpr) if c else None))

    def load_blob(self, path):
        """[u32 va, u32 size, bytes...] little-endian headers (export_golden.py format)."""
        tot = 0
        with open(path, "rb") as f:
            while True:
                hdr = f.read(8)
                if len(hdr) < 8:
                    break
                va, size = struct.unpack("<II", hdr)
                data = f.read(size)
                self.write_bytes(va, data, track=False)
                tot += size
        return tot

    def page(self, a):
        p = self.pages.get(a >> PAGE_BITS)
        if p is None:
            p = bytearray(PAGE)
            self.pages[a >> PAGE_BITS] = p
        return p

    def snapshot(self):
        """Remember the current content of every page as pristine (for restore())."""
        self.pristine = {}
        self.dirty = set()

    def _touch(self, pn):
        if pn not in self.dirty:
            self.dirty.add(pn)
            p = self.pages.get(pn)
            self.pristine[pn] = bytes(p) if p is not None else None

    def restore(self):
        for pn in self.dirty:
            old = self.pristine.get(pn)
            if old is None:
                self.pages.pop(pn, None)
            else:
                self.pages[pn][:] = old
        self.dirty = set()
        self.pristine = {}

    def read_bytes(self, a, n):
        out = bytearray()
        while n > 0:
            off = a & PMASK
            k = min(n, PAGE - off)
            p = self.pages.get(a >> PAGE_BITS)
            if p is None:
                self.oob_reads += 1
                out += bytes(k)
            else:
                out += p[off:off + k]
            a = (a + k) & M32
            n -= k
        return bytes(out)

    def write_bytes(self, a, data, track=True):
        i = 0
        n = len(data)
        if self.watch:
            a0, n0 = a, n
            self._write_bytes(a, data, track)
            self._wh(a0, n0)
            return
        self._write_bytes(a, data, track)

    def _write_bytes(self, a, data, track=True):
        i = 0
        n = len(data)
        while i < n:
            off = a & PMASK
            k = min(n - i, PAGE - off)
            pn = a >> PAGE_BITS
            if track:
                self._touch(pn)
            self.page(a)[off:off + k] = data[i:i + k]
            a = (a + k) & M32
            i += k

    def r8(self, a):
        p = self.pages.get(a >> PAGE_BITS)
        if p is None:
            self.oob_reads += 1
            return 0
        return p[a & PMASK]

    def r16(self, a):
        off = a & PMASK
        if off <= PAGE - 2:
            p = self.pages.get(a >> PAGE_BITS)
            if p is None:
                self.oob_reads += 1
                return 0
            return U16.unpack_from(p, off)[0]
        return U16.unpack(self.read_bytes(a, 2))[0]

    def r32(self, a):
        off = a & PMASK
        if off <= PAGE - 4:
            p = self.pages.get(a >> PAGE_BITS)
            if p is None:
                self.oob_reads += 1
                return 0
            return U32.unpack_from(p, off)[0]
        return U32.unpack(self.read_bytes(a, 4))[0]

    def r64(self, a):
        off = a & PMASK
        if off <= PAGE - 8:
            p = self.pages.get(a >> PAGE_BITS)
            if p is None:
                self.oob_reads += 1
                return 0
            return U64.unpack_from(p, off)[0]
        return U64.unpack(self.read_bytes(a, 8))[0]

    def w8(self, a, v):
        pn = a >> PAGE_BITS
        if pn not in self.dirty:
            self._touch(pn)
        self.page(a)[a & PMASK] = v & 0xFF
        if self.watch:
            self._wh(a, 1)

    def w16(self, a, v):
        off = a & PMASK
        if off <= PAGE - 2:
            pn = a >> PAGE_BITS
            if pn not in self.dirty:
                self._touch(pn)
            U16.pack_into(self.page(a), off, v & 0xFFFF)
            if self.watch:
                self._wh(a, 2)
        else:
            self.write_bytes(a, U16.pack(v & 0xFFFF))

    def w32(self, a, v):
        off = a & PMASK
        if off <= PAGE - 4:
            pn = a >> PAGE_BITS
            if pn not in self.dirty:
                self._touch(pn)
            U32.pack_into(self.page(a), off, v & M32)
            if self.watch:
                self._wh(a, 4)
        else:
            self.write_bytes(a, U32.pack(v & M32))

    def w64(self, a, v):
        off = a & PMASK
        if off <= PAGE - 8:
            pn = a >> PAGE_BITS
            if pn not in self.dirty:
                self._touch(pn)
            U64.pack_into(self.page(a), off, v & M64)
            if self.watch:
                self._wh(a, 8)
        else:
            self.write_bytes(a, U64.pack(v & M64))


# ----------------------------------------------------------------------------------------------
# float helpers
# ----------------------------------------------------------------------------------------------
def f32_bits(x):
    """Round a Python float (double) to single, return the u32 bit pattern (RNE, IEEE)."""
    return int(np.float32(x).view(np.uint32))


def bits_f32(u):
    return float(np.uint32(u).view(np.float32))


def d_bits(x):
    return U64.unpack(F64.pack(x))[0]


def bits_d(u):
    return F64.unpack(U64.pack(u & M64))[0]


def is_single_midpoint(x):
    """True when double x lies exactly halfway between two adjacent singles (a RNE tie for the
    double->single rounding), i.e. when the rounding of an exact result could depend on bits
    below the double's precision."""
    if x == 0.0 or math.isinf(x) or math.isnan(x):
        return False
    b = d_bits(x)
    exp = (b >> 52) & 0x7FF
    if exp == 0:
        return False
    # single keeps 23 fraction bits for normal singles; for single-subnormal range fewer.
    e_unb = exp - 1023
    if e_unb >= -126:
        drop = 52 - 23
    else:
        drop = 52 - 23 + (-126 - e_unb)
        if drop > 53:
            return False
    frac = (b & ((1 << 52) - 1)) | (1 << 52)
    low = frac & ((1 << drop) - 1)
    return low == (1 << (drop - 1))


def round_exact_to_single(exact, approx):
    """exact: Fraction (the mathematically exact result); approx: the correctly rounded double of
    it. Return the correctly rounded single (as a Python float)."""
    if not is_single_midpoint(approx) or Fraction(approx) == exact:
        # not a tie, or the double IS the exact value (a true tie): numpy's RNE is correct
        return float(np.float32(approx))
    # the double is a single-precision midpoint only because it was rounded: decide the side
    # with the exact value
    lo = float(np.float32(approx))
    if Fraction(lo) == exact:
        return lo
    # the two single neighbours around approx
    a32 = np.float32(approx)
    down = float(np.nextafter(a32, np.float32(-np.inf)))
    up = float(np.nextafter(a32, np.float32(np.inf)))
    fa = float(a32)
    if fa < approx:
        cand_lo, cand_hi = fa, up
    else:
        cand_lo, cand_hi = down, fa
    return cand_hi if exact > Fraction(approx) else cand_lo


def single_op(op, a, b):
    """Correctly rounded single-precision result of a op b for double inputs."""
    if op == "add":
        r = a + b
    elif op == "sub":
        r = a - b
    elif op == "mul":
        r = a * b
    else:
        if b == 0.0 or math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
            return float(np.float32(np.float64(a) / np.float64(b)))
        r = a / b
    if math.isnan(r) or math.isinf(r):
        return float(np.float32(np.float64(a) + 0.0 if op == "add" and False else r))
    if not is_single_midpoint(r):
        return float(np.float32(r))
    fa, fb = Fraction(a), Fraction(b)
    ex = {"add": fa + fb, "sub": fa - fb, "mul": fa * fb, "div": fa / fb}[op]
    return round_exact_to_single(ex, r)


def fused_single(a, c, b, neg_prod_sub):
    """Exact (a*c + b) or (a*c - b) rounded once to single (fmadds/fmsubs)."""
    if any(math.isnan(x) or math.isinf(x) for x in (a, c, b)):
        v = np.float64(a) * np.float64(c)
        v = v - np.float64(b) if neg_prod_sub else v + np.float64(b)
        return float(np.float32(v))
    ex = Fraction(a) * Fraction(c)
    ex = ex - Fraction(b) if neg_prod_sub else ex + Fraction(b)
    if ex == 0:
        # sign of an exact zero sum: RNE gives +0 unless both addends are -0
        p = a * c
        s = -b if neg_prod_sub else b
        return -0.0 if (math.copysign(1, p) < 0 and math.copysign(1, s) < 0) else 0.0
    approx = ex.numerator / ex.denominator  # correctly rounded (Python int true division)
    if math.isinf(approx):
        return approx
    return round_exact_to_single(ex, approx)


def fused_double(a, c, b, sub):
    """a*c +- b with ONE rounding to double (x64 vfmadd213sd)."""
    if any(math.isnan(x) or math.isinf(x) for x in (a, c, b)):
        v = np.float64(a) * np.float64(c)
        return float(v - np.float64(b) if sub else v + np.float64(b))
    ex = Fraction(a) * Fraction(c)
    ex = ex - Fraction(b) if sub else ex + Fraction(b)
    if ex == 0:
        p = a * c
        s = -b if sub else b
        return -0.0 if (math.copysign(1, p) < 0 and math.copysign(1, s) < 0) else 0.0
    try:
        return ex.numerator / ex.denominator
    except OverflowError:
        return math.inf if ex > 0 else -math.inf


def fused_f32_vec(a, c, b):
    """Per-lane exact a*c+b rounded once to single; a,c,b are float32 numpy arrays."""
    out = np.empty(4, dtype=np.float32)
    for i in range(4):
        x, y, z = float(a[i]), float(c[i]), float(b[i])
        if any(math.isnan(t) or math.isinf(t) for t in (x, y, z)):
            out[i] = np.float32(np.float64(x) * np.float64(y) + np.float64(z))
            continue
        ex = Fraction(x) * Fraction(y) + Fraction(z)
        if ex == 0:
            p = x * y
            out[i] = np.float32(-0.0 if (math.copysign(1, p) < 0 and math.copysign(1, z) < 0) else 0.0)
            continue
        approx = ex.numerator / ex.denominator if abs(ex) < 1e300 else float("inf") * (1 if ex > 0 else -1)
        out[i] = np.float32(round_exact_to_single(ex, approx)) if not math.isinf(approx) else np.float32(approx)
    return out


def flush_denorm(v):
    """v: float32 array; flush subnormals to signed zero (VMX non-Java mode)."""
    u = v.view(np.uint32)
    sub = ((u & 0x7F800000) == 0) & ((u & 0x007FFFFF) != 0)
    if sub.any():
        u = np.where(sub, u & 0x80000000, u).astype(np.uint32)
        return u.view(np.float32)
    return v


# ----------------------------------------------------------------------------------------------
# CPU
# ----------------------------------------------------------------------------------------------
def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def sx32(v):
    v &= M32
    return v - 0x100000000 if v & 0x80000000 else v


def sx64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


def rotl32(v, n):
    v &= M32
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M32 if n else v


def rotl64(v, n):
    v &= M64
    n &= 63
    return ((v << n) | (v >> (64 - n))) & M64 if n else v


def mask64(mb, me):
    """PowerPC MASK(mb, me), bit 0 = MSB of 64."""
    if mb <= me:
        m = ((1 << (64 - mb)) - 1) & ~((1 << (63 - me)) - 1)
    else:
        m = ((1 << (64 - mb)) - 1) | (M64 & ~((1 << (63 - me)) - 1))
    return m & M64


class CPU:
    def __init__(self, mem, mode="xenia"):
        self.mem = mem
        self.mode = mode
        self.gpr = [0] * 32
        self.fpr = [0.0] * 32
        self.vr = [[0, 0, 0, 0] for _ in range(128)]
        self.cr = [0] * 8          # each field: bit3 LT, bit2 GT, bit1 EQ, bit0 SO/UN
        self.ca = 0
        self.so = 0
        self.lr = 0
        self.ctr = 0
        self.pc = 0
        self.steps = 0
        self.tb = 0
        self.hooks = {}            # VA -> python fn(cpu); called on bl/bctrl/bctr targets
        self.cache = {}
        self.stats = {}            # mnemonic -> count (coverage)
        self.branch_cov = {}       # branch VA -> [taken, not-taken]
        self.log = []              # side-channel notes (not compared)
        self.cr0_width_mismatch = 0
        self.ca_width_mismatch = 0
        self.fpscr = 0             # modelled FPSCR (mffs / mtfsf); RN must stay 0
        self.msr = 0

    # --- CR helpers -------------------------------------------------------------------------
    def set_cr_cmp(self, field, a, b):
        self.cr[field] = (8 if a < b else 4 if a > b else 2) | self.so

    def cr0_record(self, val):
        s64 = sx64(val)
        s32 = sx32(val)
        if (s64 < 0) != (s32 < 0) or (s64 == 0) != (s32 == 0):
            self.cr0_width_mismatch += 1
        v = s64 if self.mode == "isa" else s32   # Xenia UpdateCR truncates to i32; so does XenonRecomp
        self.cr[0] = (8 if v < 0 else 4 if v > 0 else 2) | self.so

    def cr_bit(self, bi):
        return (self.cr[bi >> 2] >> (3 - (bi & 3))) & 1

    def set_cr_bit(self, bi, v):
        f = bi >> 2
        sh = 3 - (bi & 3)
        self.cr[f] = (self.cr[f] & ~(1 << sh)) | ((v & 1) << sh)

    # --- vector helpers ---------------------------------------------------------------------
    def vf(self, r):
        return np.array(self.vr[r], dtype=np.uint32).view(np.float32)

    def set_vf(self, r, arr):
        self.vr[r] = [int(x) for x in np.asarray(arr, dtype=np.float32).view(np.uint32)]

    # --- run --------------------------------------------------------------------------------
    def call(self, va, max_steps=50_000_000, sentinel=0x7FFFFFF0):
        self.lr = sentinel
        self.pc = va
        cache = self.cache
        steps = 0
        pc = va
        mem = self.mem
        while pc != sentinel:
            fn = cache.get(pc)
            if fn is None:
                fn = self.decode(pc, mem.r32(pc))
                cache[pc] = fn
            self.pc = pc
            npc = fn(self)
            pc = pc + 4 if npc is None else npc
            steps += 1
            if steps > max_steps:
                raise GuestFault("step limit at %08X" % pc)
        self.steps += steps
        return steps

    def branch_to(self, target, link_pc):
        """bl / bctrl / bctr target resolution: python hooks stand in for imports and the
        harness collector. Returns the next pc."""
        h = self.hooks.get(target)
        if h is not None:
            if link_pc is not None:
                self.lr = link_pc
            h(self)
            return self.lr
        if link_pc is not None:
            self.lr = link_pc
        return target

    def note_branch(self, pc, taken):
        c = self.branch_cov.get(pc)
        if c is None:
            c = self.branch_cov[pc] = [0, 0]
        c[0 if taken else 1] += 1

    # --- decoder ----------------------------------------------------------------------------
    def decode(self, pc, w):
        op = w >> 26
        d = DECODERS.get(op)
        if d is None:
            raise Unimplemented("opcode %d word %08X at %08X" % (op, w, pc))
        fn, name = d(w, pc)
        if fn is None:
            raise Unimplemented("%s word %08X at %08X" % (name, w, pc))
        stats = self.stats

        def counted(cpu, _fn=fn, _name=name):
            stats[_name] = stats.get(_name, 0) + 1
            return _fn(cpu)
        return counted


# ================================================================================================
# instruction implementations (factory functions returning (closure, mnemonic))
# ================================================================================================
DECODERS = {}
MODE = ["xenia"]


def fields(w):
    return (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31


# ---- D-form integer ---------------------------------------------------------------------------
def dec_addi(w, pc):
    rt, ra, _ = fields(w)
    si = sx16(w & 0xFFFF)
    if ra == 0:
        v = si & M64
        def f(c): c.gpr[rt] = v
    else:
        def f(c): c.gpr[rt] = (c.gpr[ra] + si) & M64
    return f, "addi"


def dec_addis(w, pc):
    rt, ra, _ = fields(w)
    si = sx16(w & 0xFFFF) << 16
    if ra == 0:
        v = si & M64
        def f(c): c.gpr[rt] = v
    else:
        def f(c): c.gpr[rt] = (c.gpr[ra] + si) & M64
    return f, "addis"


def _carry_add(c, a, b, cin=0):
    """64-bit add with carry; recomp mode computes CA on the low 32 bits."""
    r = a + b + cin
    ca64 = 1 if r > M64 else 0
    ca32 = 1 if (a & M32) + (b & M32) + cin > M32 else 0
    if ca64 != ca32:
        c.ca_width_mismatch += 1
    c.ca = ca32 if c.mode == "recomp" else ca64
    return r & M64


def dec_addic(w, pc, rec=False):
    rt, ra, _ = fields(w)
    si = sx16(w & 0xFFFF) & M64
    def f(c):
        r = _carry_add(c, c.gpr[ra], si)
        c.gpr[rt] = r
        if rec:
            c.cr0_record(r)
    return f, "addic." if rec else "addic"


def dec_subfic(w, pc):
    rt, ra, _ = fields(w)
    si = sx16(w & 0xFFFF) & M64
    def f(c):
        c.gpr[rt] = _carry_add(c, (~c.gpr[ra]) & M64, si, 1)
    return f, "subfic"


def dec_mulli(w, pc):
    rt, ra, _ = fields(w)
    si = sx16(w & 0xFFFF)
    def f(c): c.gpr[rt] = (sx64(c.gpr[ra]) * si) & M64
    return f, "mulli"


def dec_cmpi(w, pc):
    bf = (w >> 23) & 7
    l = (w >> 21) & 1
    ra = (w >> 16) & 31
    si = sx16(w & 0xFFFF)
    if l:
        def f(c): c.set_cr_cmp(bf, sx64(c.gpr[ra]), si)
    else:
        def f(c): c.set_cr_cmp(bf, sx32(c.gpr[ra]), si)
    return f, "cmpdi" if l else "cmpwi"


def dec_cmpli(w, pc):
    bf = (w >> 23) & 7
    l = (w >> 21) & 1
    ra = (w >> 16) & 31
    ui = w & 0xFFFF
    if l:
        def f(c): c.set_cr_cmp(bf, c.gpr[ra], ui)
    else:
        def f(c): c.set_cr_cmp(bf, c.gpr[ra] & M32, ui)
    return f, "cmpldi" if l else "cmplwi"


def dec_logic_imm(kind):
    def d(w, pc):
        rs, ra, _ = fields(w)
        ui = w & 0xFFFF
        if kind == "ori":
            def f(c): c.gpr[ra] = c.gpr[rs] | ui
        elif kind == "oris":
            def f(c): c.gpr[ra] = c.gpr[rs] | (ui << 16)
        elif kind == "xori":
            def f(c): c.gpr[ra] = c.gpr[rs] ^ ui
        elif kind == "xoris":
            def f(c): c.gpr[ra] = c.gpr[rs] ^ (ui << 16)
        elif kind == "andi.":
            def f(c):
                r = c.gpr[rs] & ui
                c.gpr[ra] = r
                c.cr0_record(r)
        else:  # andis.
            def f(c):
                r = c.gpr[rs] & (ui << 16)
                c.gpr[ra] = r
                c.cr0_record(r)
        return f, kind
    return d


# ---- rotates ------------------------------------------------------------------------------------
def dec_rlwinm(w, pc):
    rs, ra, sh = fields(w)
    mb = (w >> 6) & 31
    me = (w >> 1) & 31
    rc = w & 1
    m = mask64(mb + 32, me + 32)
    def f(c):
        v = c.gpr[rs] & M32
        r = rotl32(v, sh)
        r = (r | (r << 32)) & m
        c.gpr[ra] = r
        if rc:
            c.cr0_record(r)
    return f, "rlwinm." if rc else "rlwinm"


def dec_rlwnm(w, pc):
    rs, ra, rb = fields(w)
    mb = (w >> 6) & 31
    me = (w >> 1) & 31
    rc = w & 1
    m = mask64(mb + 32, me + 32)
    def f(c):
        r = rotl32(c.gpr[rs] & M32, c.gpr[rb] & 31)
        r = (r | (r << 32)) & m
        c.gpr[ra] = r
        if rc:
            c.cr0_record(r)
    return f, "rlwnm." if rc else "rlwnm"


def dec_rlwimi(w, pc):
    rs, ra, sh = fields(w)
    mb = (w >> 6) & 31
    me = (w >> 1) & 31
    rc = w & 1
    m = mask64(mb + 32, me + 32)
    def f(c):
        r = rotl32(c.gpr[rs] & M32, sh)
        r = (r | (r << 32))
        v = (r & m) | (c.gpr[ra] & ~m & M64)
        c.gpr[ra] = v
        if rc:
            c.cr0_record(v)
    return f, "rlwimi." if rc else "rlwimi"


def dec_op30(w, pc):
    rs, ra, _ = fields(w)
    sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
    mbe = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)   # mb/me field: low 5 bits then high bit
    xo = (w >> 2) & 7
    rc = w & 1
    if xo == 0:    # rldicl
        m = mask64(mbe, 63)
        def f(c):
            r = rotl64(c.gpr[rs], sh) & m
            c.gpr[ra] = r
            if rc:
                c.cr0_record(r)
        return f, "rldicl"
    if xo == 1:    # rldicr
        m = mask64(0, mbe)
        def f(c):
            r = rotl64(c.gpr[rs], sh) & m
            c.gpr[ra] = r
            if rc:
                c.cr0_record(r)
        return f, "rldicr"
    if xo == 2:    # rldic
        m = mask64(mbe, 63 - sh)
        def f(c):
            r = rotl64(c.gpr[rs], sh) & m
            c.gpr[ra] = r
            if rc:
                c.cr0_record(r)
        return f, "rldic"
    if xo == 3:    # rldimi
        m = mask64(mbe, 63 - sh)
        def f(c):
            r = (rotl64(c.gpr[rs], sh) & m) | (c.gpr[ra] & ~m & M64)
            c.gpr[ra] = r
            if rc:
                c.cr0_record(r)
        return f, "rldimi"
    return None, "op30/%d" % xo


# ---- branches -------------------------------------------------------------------------------
def dec_b(w, pc):
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    aa = (w >> 1) & 1
    lk = w & 1
    tgt = (li if aa else pc + li) & M32
    if lk:
        def f(c): return c.branch_to(tgt, (pc + 4) & M32)
        return f, "bl"
    # a tail branch into a hooked VA runs the hook too (it stands in for the function there)
    def f(c): return c.branch_to(tgt, None) if tgt in c.hooks else tgt
    return f, "b"


def _bc_cond(bo, bi):
    """returns fn(c)->bool evaluating BO/BI (decrements CTR when BO says so)."""
    dec = not (bo & 4)
    test_cond = not (bo & 16)
    def cond(c):
        ok = True
        if dec:
            c.ctr = (c.ctr - 1) & M64
            z = (c.ctr & M32) == 0     # 32-bit mode? Xenon runs SF=1; CTR compare 64-bit
            z = c.ctr == 0
            ok = z if (bo & 2) else not z
        if test_cond and ok:
            ok = c.cr_bit(bi) == ((bo >> 3) & 1)
        return ok
    return cond


def dec_bc(w, pc):
    bo = (w >> 21) & 31
    bi = (w >> 16) & 31
    bd = w & 0xFFFC
    if bd & 0x8000:
        bd -= 0x10000
    aa = (w >> 1) & 1
    lk = w & 1
    tgt = (bd if aa else pc + bd) & M32
    cond = _bc_cond(bo, bi)
    def f(c):
        t = cond(c)
        c.note_branch(pc, t)
        if t:
            if lk:
                return c.branch_to(tgt, (pc + 4) & M32)
            return tgt
        if lk:
            c.lr = (pc + 4) & M32
        return None
    return f, "bc"


def dec_op19(w, pc):
    xo = (w >> 1) & 0x3FF
    bo = (w >> 21) & 31
    bi = (w >> 16) & 31
    lk = w & 1
    if xo == 16:   # bclr
        cond = _bc_cond(bo, bi)
        def f(c):
            t = cond(c)
            c.note_branch(pc, t)
            if t:
                tgt = c.lr & 0xFFFFFFFC
                if lk:
                    c.lr = (pc + 4) & M32
                return tgt
            if lk:
                c.lr = (pc + 4) & M32
            return None
        return f, "bclr"
    if xo == 528:  # bcctr
        cond = _bc_cond(bo | 4, bi)   # bcctr never decrements
        def f(c):
            t = cond(c)
            if t:
                tgt = c.ctr & 0xFFFFFFFC
                return c.branch_to(tgt, (pc + 4) & M32 if lk else None)
            if lk:
                c.lr = (pc + 4) & M32
            return None
        return f, "bcctrl" if lk else "bcctr"
    if xo == 0:    # mcrf
        bf = (w >> 23) & 7
        bfa = (w >> 18) & 7
        def f(c): c.cr[bf] = c.cr[bfa]
        return f, "mcrf"
    crops = {257: lambda a, b: a & b, 449: lambda a, b: a | b, 193: lambda a, b: a ^ b,
             225: lambda a, b: 1 - (a & b), 33: lambda a, b: 1 - (a | b), 289: lambda a, b: 1 - (a ^ b),
             129: lambda a, b: a & (1 - b), 417: lambda a, b: a | (1 - b)}
    if xo in crops:
        bt, ba, bb = fields(w)
        op = crops[xo]
        def f(c): c.set_cr_bit(bt, op(c.cr_bit(ba), c.cr_bit(bb)))
        return f, "crop%d" % xo
    if xo == 150:
        return (lambda c: None), "isync"
    return None, "op19/%d" % xo


# ---- loads / stores D-form -----------------------------------------------------------------
def _ea_d(w):
    rt, ra, _ = fields(w)
    d = sx16(w & 0xFFFF)
    return rt, ra, d


def dec_load(kind, update=False):
    def d(w, pc):
        rt, ra, disp = _ea_d(w)
        def ea(c):
            return ((c.gpr[ra] if ra else 0) + disp) & M32
        if kind == "lwz":
            def f(c):
                a = ea(c); c.gpr[rt] = c.mem.r32(a)
                if update: c.gpr[ra] = a
        elif kind == "lbz":
            def f(c):
                a = ea(c); c.gpr[rt] = c.mem.r8(a)
                if update: c.gpr[ra] = a
        elif kind == "lhz":
            def f(c):
                a = ea(c); c.gpr[rt] = c.mem.r16(a)
                if update: c.gpr[ra] = a
        elif kind == "lha":
            def f(c):
                a = ea(c); c.gpr[rt] = sx16(c.mem.r16(a)) & M64
                if update: c.gpr[ra] = a
        elif kind == "lfs":
            def f(c):
                a = ea(c); c.fpr[rt] = bits_f32(c.mem.r32(a))
                if update: c.gpr[ra] = a
        elif kind == "lfd":
            def f(c):
                a = ea(c); c.fpr[rt] = bits_d(c.mem.r64(a))
                if update: c.gpr[ra] = a
        elif kind == "stw":
            def f(c):
                a = ea(c); c.mem.w32(a, c.gpr[rt])
                if update: c.gpr[ra] = a
        elif kind == "stb":
            def f(c):
                a = ea(c); c.mem.w8(a, c.gpr[rt])
                if update: c.gpr[ra] = a
        elif kind == "sth":
            def f(c):
                a = ea(c); c.mem.w16(a, c.gpr[rt])
                if update: c.gpr[ra] = a
        elif kind == "stfs":
            def f(c):
                a = ea(c); c.mem.w32(a, store_single(c, c.fpr[rt]))
                if update: c.gpr[ra] = a
        elif kind == "stfd":
            def f(c):
                a = ea(c); c.mem.w64(a, d_bits(c.fpr[rt]))
                if update: c.gpr[ra] = a
        elif kind == "lmw":
            def f(c):
                a = ea(c)
                for r in range(rt, 32):
                    c.gpr[r] = c.mem.r32(a); a += 4
        elif kind == "stmw":
            def f(c):
                a = ea(c)
                for r in range(rt, 32):
                    c.mem.w32(a, c.gpr[r]); a += 4
        else:
            return None, kind
        return f, kind + ("u" if update else "")
    return d


def store_single(c, x):
    """stfs: ISA converts the double to single format WITHOUT rounding for values in single
    range (the double's low fraction bits are dropped); recomp mode rounds (float(f64))."""
    if c.mode != "isa":     # Xenia: Convert(FLOAT32) (cvtsd2ss, rounds); XenonRecomp: float(f64)
        return f32_bits(x)
    b = d_bits(x)
    exp = (b >> 52) & 0x7FF
    if exp > 896 or exp == 0x7FF:     # normal in single range, or inf/nan
        # ISA: WORD0:1 = FRS0:1, WORD2:31 = FRS5:34
        return ((b >> 32) & 0xC0000000) | ((b >> 29) & 0x3FFFFFFF)
    if exp == 0:
        return (b >> 32) & 0x80000000
    # denormalize (single subnormal)
    sign = (b >> 63) << 31
    frac = (b & ((1 << 52) - 1)) | (1 << 52)
    shift = 897 - exp
    m = frac >> (29 + shift)
    return sign | (m & 0x7FFFFF)


def dec_op58(w, pc):
    rt, ra, _ = fields(w)
    ds = sx16(w & 0xFFFC)
    xo = w & 3
    def ea(c):
        return ((c.gpr[ra] if ra else 0) + ds) & M32
    if xo == 0:
        def f(c): c.gpr[rt] = c.mem.r64(ea(c))
        return f, "ld"
    if xo == 1:
        def f(c):
            a = ea(c); c.gpr[rt] = c.mem.r64(a); c.gpr[ra] = a
        return f, "ldu"
    if xo == 2:
        def f(c): c.gpr[rt] = sx32(c.mem.r32(ea(c))) & M64
        return f, "lwa"
    return None, "op58"


def dec_op62(w, pc):
    rs, ra, _ = fields(w)
    ds = sx16(w & 0xFFFC)
    xo = w & 3
    def ea(c):
        return ((c.gpr[ra] if ra else 0) + ds) & M32
    if xo == 0:
        def f(c): c.mem.w64(ea(c), c.gpr[rs])
        return f, "std"
    if xo == 1:
        def f(c):
            a = ea(c); c.mem.w64(a, c.gpr[rs]); c.gpr[ra] = a
        return f, "stdu"
    return None, "op62"


# ---- opcode 31 --------------------------------------------------------------------------------
def dec_op31(w, pc):
    rt, ra, rb = fields(w)
    xo10 = (w >> 1) & 0x3FF
    xo9 = (w >> 1) & 0x1FF
    rc = w & 1
    oe = (w >> 10) & 1

    def eax(c):
        return ((c.gpr[ra] if ra else 0) + c.gpr[rb]) & M32

    def rec(fn, name):
        if rc:
            def g(c):
                fn(c)
                c.cr0_record(c.gpr[rt])
            return g, name + "."
        return fn, name

    def rec_a(fn, name):   # record on RA (logical ops)
        if rc:
            def g(c):
                fn(c)
                c.cr0_record(c.gpr[ra])
            return g, name + "."
        return fn, name

    # XO-form arithmetic (9-bit XO)
    if xo9 == 266:
        def f(c): c.gpr[rt] = (c.gpr[ra] + c.gpr[rb]) & M64
        return rec(f, "add")
    if xo9 == 40:
        def f(c): c.gpr[rt] = (c.gpr[rb] - c.gpr[ra]) & M64
        return rec(f, "subf")
    if xo9 == 8:
        def f(c): c.gpr[rt] = _carry_add(c, (~c.gpr[ra]) & M64, c.gpr[rb], 1)
        return rec(f, "subfc")
    if xo9 == 136:
        def f(c): c.gpr[rt] = _carry_add(c, (~c.gpr[ra]) & M64, c.gpr[rb], c.ca)
        return rec(f, "subfe")
    if xo9 == 10:
        def f(c): c.gpr[rt] = _carry_add(c, c.gpr[ra], c.gpr[rb])
        return rec(f, "addc")
    if xo9 == 138:
        def f(c): c.gpr[rt] = _carry_add(c, c.gpr[ra], c.gpr[rb], c.ca)
        return rec(f, "adde")
    if xo9 == 202:
        def f(c): c.gpr[rt] = _carry_add(c, c.gpr[ra], 0, c.ca)
        return rec(f, "addze")
    if xo9 == 200:
        def f(c): c.gpr[rt] = _carry_add(c, (~c.gpr[ra]) & M64, 0, c.ca)
        return rec(f, "subfze")
    if xo9 == 234:
        def f(c): c.gpr[rt] = _carry_add(c, c.gpr[ra], M64, c.ca)
        return rec(f, "addme")
    if xo9 == 104:
        def f(c): c.gpr[rt] = (-c.gpr[ra]) & M64
        return rec(f, "neg")
    if xo9 == 235:
        def f(c): c.gpr[rt] = (sx32(c.gpr[ra]) * sx32(c.gpr[rb])) & M64
        return rec(f, "mullw")
    if xo9 == 233:
        def f(c): c.gpr[rt] = (sx64(c.gpr[ra]) * sx64(c.gpr[rb])) & M64
        return rec(f, "mulld")
    if xo9 == 75:
        def f(c): c.gpr[rt] = ((sx32(c.gpr[ra]) * sx32(c.gpr[rb])) >> 32) & M64
        return rec(f, "mulhw")
    if xo9 == 11:
        def f(c): c.gpr[rt] = (((c.gpr[ra] & M32) * (c.gpr[rb] & M32)) >> 32) & M64
        return rec(f, "mulhwu")
    if xo9 == 491:
        def f(c):
            a, b = sx32(c.gpr[ra]), sx32(c.gpr[rb])
            if b == 0 or (a == -0x80000000 and b == -1):
                c.gpr[rt] = 0     # undefined on hardware; logged
                c.log.append("divw undefined at %08X" % pc)
            else:
                q = abs(a) // abs(b)
                if (a < 0) != (b < 0):
                    q = -q
                c.gpr[rt] = q & M32   # high word undefined by the ISA; zero-extend
        return rec(f, "divw")
    if xo9 == 459:
        def f(c):
            a, b = c.gpr[ra] & M32, c.gpr[rb] & M32
            c.gpr[rt] = (a // b) if b else 0
        return rec(f, "divwu")
    if xo9 == 489:
        def f(c):
            a, b = sx64(c.gpr[ra]), sx64(c.gpr[rb])
            if b == 0:
                c.gpr[rt] = 0
            else:
                q = abs(a) // abs(b)
                if (a < 0) != (b < 0):
                    q = -q
                c.gpr[rt] = q & M64
        return rec(f, "divd")

    if xo10 == 0 or xo10 == 32:   # cmp / cmpl
        bf = (w >> 23) & 7
        l = (w >> 21) & 1
        if xo10 == 0:
            if l:
                def f(c): c.set_cr_cmp(bf, sx64(c.gpr[ra]), sx64(c.gpr[rb]))
            else:
                def f(c): c.set_cr_cmp(bf, sx32(c.gpr[ra]), sx32(c.gpr[rb]))
            return f, "cmpd" if l else "cmpw"
        if l:
            def f(c): c.set_cr_cmp(bf, c.gpr[ra], c.gpr[rb])
        else:
            def f(c): c.set_cr_cmp(bf, c.gpr[ra] & M32, c.gpr[rb] & M32)
        return f, "cmpld" if l else "cmplw"

    rs = rt
    if xo10 == 28:
        def f(c): c.gpr[ra] = c.gpr[rs] & c.gpr[rb]
        return rec_a(f, "and")
    if xo10 == 60:
        def f(c): c.gpr[ra] = c.gpr[rs] & ~c.gpr[rb] & M64
        return rec_a(f, "andc")
    if xo10 == 444:
        def f(c): c.gpr[ra] = c.gpr[rs] | c.gpr[rb]
        return rec_a(f, "or")
    if xo10 == 412:
        def f(c): c.gpr[ra] = (c.gpr[rs] | (~c.gpr[rb] & M64)) & M64
        return rec_a(f, "orc")
    if xo10 == 316:
        def f(c): c.gpr[ra] = c.gpr[rs] ^ c.gpr[rb]
        return rec_a(f, "xor")
    if xo10 == 124:
        def f(c): c.gpr[ra] = ~(c.gpr[rs] | c.gpr[rb]) & M64
        return rec_a(f, "nor")
    if xo10 == 476:
        def f(c): c.gpr[ra] = ~(c.gpr[rs] & c.gpr[rb]) & M64
        return rec_a(f, "nand")
    if xo10 == 284:
        def f(c): c.gpr[ra] = ~(c.gpr[rs] ^ c.gpr[rb]) & M64
        return rec_a(f, "eqv")
    if xo10 == 26:
        def f(c):
            v = c.gpr[rs] & M32
            c.gpr[ra] = 32 - v.bit_length()
        return rec_a(f, "cntlzw")
    if xo10 == 58:
        def f(c): c.gpr[ra] = 64 - c.gpr[rs].bit_length()
        return rec_a(f, "cntlzd")
    if xo10 == 954:
        def f(c):
            v = c.gpr[rs] & 0xFF
            c.gpr[ra] = (v - 0x100 if v & 0x80 else v) & M64
        return rec_a(f, "extsb")
    if xo10 == 922:
        def f(c): c.gpr[ra] = sx16(c.gpr[rs] & 0xFFFF) & M64
        return rec_a(f, "extsh")
    if xo10 == 986:
        def f(c): c.gpr[ra] = sx32(c.gpr[rs]) & M64
        return rec_a(f, "extsw")
    if xo10 == 24:
        def f(c):
            n = c.gpr[rb] & 63
            c.gpr[ra] = ((c.gpr[rs] & M32) << n) & M32 if n < 32 else 0
        return rec_a(f, "slw")
    if xo10 == 536:
        def f(c):
            n = c.gpr[rb] & 63
            c.gpr[ra] = (c.gpr[rs] & M32) >> n if n < 32 else 0
        return rec_a(f, "srw")
    if xo10 == 792:
        def f(c):
            n = c.gpr[rb] & 63
            v = sx32(c.gpr[rs])
            if n < 32:
                r = v >> n
                c.ca = 1 if (v < 0 and (v & ((1 << n) - 1))) else 0
            else:
                r = -1 if v < 0 else 0
                c.ca = 1 if v < 0 else 0
            c.gpr[ra] = r & M64
        return rec_a(f, "sraw")
    if xo10 == 824:
        n = rb
        def f(c):
            v = sx32(c.gpr[rs])
            c.ca = 1 if (v < 0 and (v & ((1 << n) - 1))) else 0
            c.gpr[ra] = (v >> n) & M64
        return rec_a(f, "srawi")
    if xo10 == 27:
        def f(c):
            n = c.gpr[rb] & 127
            c.gpr[ra] = (c.gpr[rs] << n) & M64 if n < 64 else 0
        return rec_a(f, "sld")
    if xo10 == 539:
        def f(c):
            n = c.gpr[rb] & 127
            c.gpr[ra] = c.gpr[rs] >> n if n < 64 else 0
        return rec_a(f, "srd")
    if xo10 == 794:
        def f(c):
            n = c.gpr[rb] & 127
            v = sx64(c.gpr[rs])
            if n < 64:
                c.ca = 1 if (v < 0 and (v & ((1 << n) - 1))) else 0
                c.gpr[ra] = (v >> n) & M64
            else:
                c.ca = 1 if v < 0 else 0
                c.gpr[ra] = (-1 if v < 0 else 0) & M64
        return rec_a(f, "srad")
    if (w >> 2) & 0x1FF == 413:   # sradi (XS-form)
        n = rb | (((w >> 1) & 1) << 5)
        def f(c):
            v = sx64(c.gpr[rs])
            c.ca = 1 if (v < 0 and (v & ((1 << n) - 1))) else 0
            c.gpr[ra] = (v >> n) & M64
        return rec_a(f, "sradi")

    # indexed loads/stores
    ldst = {23: ("lwz", 0), 55: ("lwz", 1), 87: ("lbz", 0), 119: ("lbz", 1), 279: ("lhz", 0),
            311: ("lhz", 1), 343: ("lha", 0), 21: ("ld", 0), 53: ("ld", 1), 341: ("lwa", 0),
            151: ("stw", 0), 183: ("stw", 1), 215: ("stb", 0), 247: ("stb", 1), 407: ("sth", 0),
            439: ("sth", 1), 149: ("std", 0), 181: ("std", 1), 535: ("lfs", 0), 567: ("lfs", 1),
            599: ("lfd", 0), 631: ("lfd", 1), 663: ("stfs", 0), 695: ("stfs", 1), 727: ("stfd", 0),
            759: ("stfd", 1), 983: ("stfiwx", 0), 534: ("lwbrx", 0), 662: ("stwbrx", 0)}
    if xo10 in ldst:
        kind, upd = ldst[xo10]
        name = kind + ("u" if upd else "") + "x"
        def upd_ra(c, a):
            if upd:
                c.gpr[ra] = a
        if kind == "lwz":
            def f(c): a = eax(c); c.gpr[rt] = c.mem.r32(a); upd_ra(c, a)
        elif kind == "lbz":
            def f(c): a = eax(c); c.gpr[rt] = c.mem.r8(a); upd_ra(c, a)
        elif kind == "lhz":
            def f(c): a = eax(c); c.gpr[rt] = c.mem.r16(a); upd_ra(c, a)
        elif kind == "lha":
            def f(c): a = eax(c); c.gpr[rt] = sx16(c.mem.r16(a)) & M64; upd_ra(c, a)
        elif kind == "ld":
            def f(c): a = eax(c); c.gpr[rt] = c.mem.r64(a); upd_ra(c, a)
        elif kind == "lwa":
            def f(c): a = eax(c); c.gpr[rt] = sx32(c.mem.r32(a)) & M64
        elif kind == "stw":
            def f(c): a = eax(c); c.mem.w32(a, c.gpr[rt]); upd_ra(c, a)
        elif kind == "stb":
            def f(c): a = eax(c); c.mem.w8(a, c.gpr[rt]); upd_ra(c, a)
        elif kind == "sth":
            def f(c): a = eax(c); c.mem.w16(a, c.gpr[rt]); upd_ra(c, a)
        elif kind == "std":
            def f(c): a = eax(c); c.mem.w64(a, c.gpr[rt]); upd_ra(c, a)
        elif kind == "lfs":
            def f(c): a = eax(c); c.fpr[rt] = bits_f32(c.mem.r32(a)); upd_ra(c, a)
        elif kind == "lfd":
            def f(c): a = eax(c); c.fpr[rt] = bits_d(c.mem.r64(a)); upd_ra(c, a)
        elif kind == "stfs":
            def f(c): a = eax(c); c.mem.w32(a, store_single(c, c.fpr[rt])); upd_ra(c, a)
        elif kind == "stfd":
            def f(c): a = eax(c); c.mem.w64(a, d_bits(c.fpr[rt])); upd_ra(c, a)
        elif kind == "stfiwx":
            def f(c): a = eax(c); c.mem.w32(a, d_bits(c.fpr[rt]) & M32)
        elif kind == "lwbrx":
            def f(c):
                a = eax(c); v = c.mem.r32(a)
                c.gpr[rt] = int.from_bytes(v.to_bytes(4, "big"), "little")
        else:  # stwbrx
            def f(c):
                a = eax(c); v = c.gpr[rt] & M32
                c.mem.w32(a, int.from_bytes(v.to_bytes(4, "big"), "little"))
        return f, name

    if xo10 == 339 or xo10 == 467:   # mfspr / mtspr
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        if xo10 == 339:
            if spr == 8:
                def f(c): c.gpr[rt] = c.lr
                return f, "mflr"
            if spr == 9:
                def f(c): c.gpr[rt] = c.ctr
                return f, "mfctr"
            if spr == 1:
                def f(c): c.gpr[rt] = (c.so << 31) | (c.ca << 29)
                return f, "mfxer"
            return None, "mfspr%d" % spr
        if spr == 8:
            def f(c): c.lr = c.gpr[rt] & M64
            return f, "mtlr"
        if spr == 9:
            def f(c): c.ctr = c.gpr[rt] & M64
            return f, "mtctr"
        if spr == 1:
            def f(c):
                v = c.gpr[rt]; c.so = (v >> 31) & 1; c.ca = (v >> 29) & 1
            return f, "mtxer"
        return None, "mtspr%d" % spr
    if xo10 == 371:   # mftb
        def f(c):
            c.tb += 1
            c.gpr[rt] = c.tb
            c.log.append("mftb at %08X" % pc)
        return f, "mftb"
    if xo10 == 19:    # mfcr
        def f(c):
            v = 0
            for i in range(8):
                v = (v << 4) | c.cr[i]
            c.gpr[rt] = v
        return f, "mfcr"
    if xo10 == 144:   # mtcrf
        fxm = (w >> 12) & 0xFF
        def f(c):
            v = c.gpr[rt] & M32
            for i in range(8):
                if fxm & (0x80 >> i):
                    c.cr[i] = (v >> (28 - 4 * i)) & 0xF
        return f, "mtcrf"
    if xo10 in (278, 246, 86, 54, 598, 854, 982, 470):   # dcbt dcbtst dcbf dcbst sync eieio icbi dcbi
        return (lambda c: None), "cachehint%d" % xo10
    if xo10 == 1014:   # dcbz: zero the 32-byte block? Xenon cache line is 128 bytes (dcbz128 = L bit)
        l128 = (w >> 21) & 1
        size = 128 if l128 else 32
        def f(c):
            a = eax(c) & ~(size - 1) & M32
            c.mem.write_bytes(a, bytes(size))
        return f, "dcbz128" if l128 else "dcbz"
    if xo10 == 83:   # mfmsr (the recomp keeps a plain ctx.msr, zeroed per CALL)
        def f(c): c.gpr[rt] = c.msr & M64
        return f, "mfmsr"
    if xo10 == 178:  # mtmsrd rS,L=1: only EE (0x8000) and RI (0x20) change (XenonRecomp lowering)
        def f(c): c.msr = (c.gpr[rt] & 0x8020) | (c.msr & ~0x8020 & M64)
        return f, "mtmsrd"
    if xo10 == 20:   # lwarx
        def f(c): c.gpr[rt] = c.mem.r32(eax(c))
        return f, "lwarx"
    if xo10 == 150:  # stwcx.
        def f(c):
            c.mem.w32(eax(c), c.gpr[rt])
            c.cr[0] = 2 | c.so
        return f, "stwcx."
    # AltiVec memory (X-form, VR = rt)
    vd = rt
    if xo10 == 103 or xo10 == 359:   # lvx / lvxl
        def f(c):
            a = eax(c) & ~0xF
            c.vr[vd] = list(struct.unpack(">4I", c.mem.read_bytes(a, 16)))
        return f, "lvx"
    if xo10 == 231 or xo10 == 487:   # stvx / stvxl
        def f(c):
            a = eax(c) & ~0xF
            c.mem.write_bytes(a, struct.pack(">4I", *c.vr[vd]))
        return f, "stvx"
    if xo10 == 519:   # lvlx
        def f(c): lvlx(c, vd, eax(c))
        return f, "lvlx"
    if xo10 == 551:   # lvrx
        def f(c): lvrx(c, vd, eax(c))
        return f, "lvrx"
    if xo10 == 647:   # stvlx
        def f(c): stvlx(c, vd, eax(c))
        return f, "stvlx"
    if xo10 == 679:   # stvrx
        def f(c): stvrx(c, vd, eax(c))
        return f, "stvrx"
    if xo10 == 199:   # stvewx
        def f(c): stvewx(c, vd, eax(c))
        return f, "stvewx"
    if xo10 == 71:    # lvewx
        def f(c): lvewx(c, vd, eax(c))
        return f, "lvewx"
    if xo10 == 6 or xo10 == 38:   # lvsl / lvsr
        def f(c):
            sh = eax(c) & 0xF
            base = sh if xo10 == 6 else 16 - sh
            bs = bytes((base + i) & 0xFF for i in range(16))
            c.vr[vd] = list(struct.unpack(">4I", bs))
        return f, "lvsl" if xo10 == 6 else "lvsr"
    return None, "op31/%d" % xo10


# ---- VMX memory helpers (Cell/Xenon lvlx/lvrx per Xenia LoadVectorLeft/Right) ---------------
def _vbytes(c, vd):
    return bytearray(struct.pack(">4I", *c.vr[vd]))


def _set_vbytes(c, vd, bs):
    c.vr[vd] = list(struct.unpack(">4I", bytes(bs)))


def lvlx(c, vd, ea):
    sh = ea & 0xF
    block = c.mem.read_bytes(ea & ~0xF & M32, 16)
    out = bytearray(16)
    out[0:16 - sh] = block[sh:16]
    _set_vbytes(c, vd, out)


def lvrx(c, vd, ea):
    sh = ea & 0xF
    out = bytearray(16)
    if sh:
        block = c.mem.read_bytes(ea & ~0xF & M32, 16)
        out[16 - sh:16] = block[0:sh]
    _set_vbytes(c, vd, out)


def stvlx(c, vd, ea):
    sh = ea & 0xF
    bs = _vbytes(c, vd)
    c.mem.write_bytes(ea & M32, bytes(bs[0:16 - sh]))


def stvrx(c, vd, ea):
    sh = ea & 0xF
    if sh:
        bs = _vbytes(c, vd)
        c.mem.write_bytes(ea & ~0xF & M32, bytes(bs[16 - sh:16]))


def stvewx(c, vd, ea):
    a = ea & ~3 & M32
    el = (a & 0xF) >> 2
    c.mem.w32(a, c.vr[vd][el])


def lvewx(c, vd, ea):
    a = ea & ~3 & M32
    el = (a & 0xF) >> 2
    v = list(c.vr[vd])
    v[el] = c.mem.r32(a)
    c.vr[vd] = v


# ---- FPU --------------------------------------------------------------------------------------
def sop(c, op, a, b):
    """xenia / recomp: double(float(a op b)) - the op in double, then ToSingle (Xenia
    ppc_emit_fpu.cc faddsx etc.; XenonRecomp lowers identically). isa: exact single rounding."""
    if c.mode != "isa":
        with np.errstate(all="ignore"):
            if op == "add": r = np.float64(a) + np.float64(b)
            elif op == "sub": r = np.float64(a) - np.float64(b)
            elif op == "mul": r = np.float64(a) * np.float64(b)
            else: r = np.float64(a) / np.float64(b)
        return float(np.float32(r))
    return single_op(op, a, b)


def dec_op59(w, pc):
    frt, fra, frb = fields(w)
    frc = (w >> 6) & 31
    xo = (w >> 1) & 31
    rc = w & 1
    if rc:
        return None, "fp-single-record"
    if xo == 21:
        def f(c): c.fpr[frt] = sop(c, "add", c.fpr[fra], c.fpr[frb])
        return f, "fadds"
    if xo == 20:
        def f(c): c.fpr[frt] = sop(c, "sub", c.fpr[fra], c.fpr[frb])
        return f, "fsubs"
    if xo == 25:
        def f(c): c.fpr[frt] = sop(c, "mul", c.fpr[fra], c.fpr[frc])
        return f, "fmuls"
    if xo == 18:
        def f(c): c.fpr[frt] = sop(c, "div", c.fpr[fra], c.fpr[frb])
        return f, "fdivs"
    if xo == 22:
        def f(c): c.fpr[frt] = (float(np.float32(math.sqrt(c.fpr[frb]))) if c.fpr[frb] >= 0 else math.nan) if c.mode != "isa" else fsqrts(c.fpr[frb])
        return f, "fsqrts"
    if xo in (28, 29, 30, 31):   # fmsubs fmadds fnmsubs fnmadds
        sub = xo in (28, 30)
        neg = xo in (30, 31)
        def f(c):
            a, cc, b = c.fpr[fra], c.fpr[frc], c.fpr[frb]
            if c.mode == "recomp":      # double(float(a*c +- b)) unfused, in double
                r = a * cc - b if sub else a * cc + b
                r = -r if neg else r
                r = float(np.float32(r))
            elif c.mode == "xenia":     # Xenia MulAdd/MulSub in double (host FMA), then ToSingle
                r = fused_double(a, cc, b, sub)
                r = -r if neg else r
                r = float(np.float32(r))
            else:                       # isa: exact, one rounding to single
                r = fused_single(a, cc, b, sub)
                r = -r if neg else r
            c.fpr[frt] = r
        return f, {28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}[xo]
    if xo == 24:
        def f(c): c.fpr[frt] = float(np.float32(1.0 / c.fpr[frb])) if c.fpr[frb] != 0 else math.copysign(math.inf, c.fpr[frb])
        return f, "fres"
    return None, "op59/%d" % xo


def fsqrts(x):
    if x < 0 or math.isnan(x):
        return math.nan
    if math.isinf(x):
        return x
    r = math.sqrt(x)   # correctly rounded double
    if not is_single_midpoint(r):
        return float(np.float32(r))
    ex_sq = Fraction(x)
    # compare r^2 with x to find the side of the exact root
    rr = Fraction(r) * Fraction(r)
    a32 = np.float32(r)
    fa = float(a32)
    up = float(np.nextafter(a32, np.float32(np.inf)))
    down = float(np.nextafter(a32, np.float32(-np.inf)))
    lo_c, hi_c = (fa, up) if fa < r else (down, fa)
    if rr == ex_sq:
        return float(np.float32(r))
    return hi_c if ex_sq > rr else lo_c


def dec_op63(w, pc):
    frt, fra, frb = fields(w)
    frc = (w >> 6) & 31
    xo5 = (w >> 1) & 31
    xo10 = (w >> 1) & 0x3FF
    rc = w & 1
    if rc:
        return None, "fp-record"
    if xo10 == 0 or xo10 == 32:   # fcmpu / fcmpo
        bf = (w >> 23) & 7
        def f(c):
            a, b = c.fpr[fra], c.fpr[frb]
            if math.isnan(a) or math.isnan(b):
                c.cr[bf] = 1
            else:
                c.cr[bf] = 8 if a < b else 4 if a > b else 2
        return f, "fcmpu" if xo10 == 0 else "fcmpo"
    if xo10 == 12:
        def f(c): c.fpr[frt] = float(np.float32(c.fpr[frb]))
        return f, "frsp"
    if xo10 == 72:
        def f(c): c.fpr[frt] = c.fpr[frb]
        return f, "fmr"
    if xo10 == 40:
        def f(c): c.fpr[frt] = -c.fpr[frb]
        return f, "fneg"
    if xo10 == 264:
        def f(c): c.fpr[frt] = abs(c.fpr[frb])
        return f, "fabs"
    if xo10 == 136:
        def f(c): c.fpr[frt] = -abs(c.fpr[frb])
        return f, "fnabs"
    if xo10 == 846:
        def f(c): c.fpr[frt] = float(sx64(d_bits(c.fpr[frb])))   # int -> double, RNE (Python)
        return f, "fcfid"
    if xo10 in (814, 815, 14, 15):
        def f(c):
            x = c.fpr[frb]
            wide = xo10 in (814, 815)
            lo, hi = (-(1 << 63), (1 << 63) - 1) if wide else (-(1 << 31), (1 << 31) - 1)
            if math.isnan(x):
                v = lo
            else:
                if xo10 in (815, 15):
                    t = math.trunc(x) if not math.isinf(x) else (hi if x > 0 else lo)
                else:
                    t = round(x) if not math.isinf(x) else (hi if x > 0 else lo)   # RNE
                v = max(lo, min(hi, t))
            c.fpr[frt] = bits_d(v & M64 if wide else (v & M32) | 0xFFF8000000000000 & 0)
        return f, {814: "fctid", 815: "fctidz", 14: "fctiw", 15: "fctiwz"}[xo10]
    if xo5 == 23:
        def f(c):
            a = c.fpr[fra]
            c.fpr[frt] = c.fpr[frc] if (not math.isnan(a) and a >= 0.0) else c.fpr[frb]
        return f, "fsel"
    if xo5 == 21:
        def f(c): c.fpr[frt] = c.fpr[fra] + c.fpr[frb]
        return f, "fadd"
    if xo5 == 20:
        def f(c): c.fpr[frt] = c.fpr[fra] - c.fpr[frb]
        return f, "fsub"
    if xo5 == 25:
        def f(c): c.fpr[frt] = c.fpr[fra] * c.fpr[frc]
        return f, "fmul"
    if xo5 == 18:
        def f(c):
            b = c.fpr[frb]
            c.fpr[frt] = float(np.float64(c.fpr[fra]) / np.float64(b))
        return f, "fdiv"
    if xo5 == 22:
        def f(c): c.fpr[frt] = math.sqrt(c.fpr[frb]) if c.fpr[frb] >= 0 else math.nan
        return f, "fsqrt"
    if xo5 in (28, 29, 30, 31):
        sub = xo5 in (28, 30)
        neg = xo5 in (30, 31)
        def f(c):
            a, cc, b = c.fpr[fra], c.fpr[frc], c.fpr[frb]
            ex = Fraction(a) * Fraction(cc)
            ex = ex - Fraction(b) if sub else ex + Fraction(b)
            r = ex.numerator / ex.denominator
            c.fpr[frt] = -r if neg else r
        return f, {28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}[xo5]
    if xo10 == 583:   # mffs: recomp returns only RN (fpscr.loadFromHost); Xenia the stored FPSCR
        def f(c):
            c.fpr[frt] = bits_d((c.fpscr & 3) if c.mode == "recomp" else (c.fpscr & M32))
        return f, "mffs"
    if xo10 == 711:   # mtfsf FM,frB: recomp storeFromGuest sets RN only; Xenia/ISA the masked fields
        fm = (w >> 17) & 0xFF
        def f(c):
            v = d_bits(c.fpr[frb]) & M32
            if c.mode == "recomp":
                c.fpscr = (c.fpscr & ~3) | (v & 3)
            else:
                m = 0
                for i in range(8):
                    if fm & (0x80 >> i):
                        m |= 0xF << (28 - 4 * i)
                c.fpscr = (c.fpscr & ~m & M32) | (v & m)
            if c.fpscr & 3:
                raise Unimplemented("mtfsf sets RN=%d (directed rounding is not modelled)" % (c.fpscr & 3))
        return f, "mtfsf"
    if xo10 in (38, 70, 134):
        return None, "fpscr-op%d" % xo10
    return None, "op63/%d" % xo10


# ---- VMX (opcode 4) and VMX128 (opcodes 4/5/6) --------------------------------------------------
def vec_bin_float(opname):
    def g(c, va, vb):
        a = flush_denorm(c.vf(va)); b = flush_denorm(c.vf(vb))
        if opname == "add":
            r = a + b
        elif opname == "sub":
            r = a - b
        elif opname == "mul":
            r = a * b
        elif opname == "max":
            if c.mode == "recomp":
                r = np.where(a > b, a, b)            # SSE maxps: second operand unless a > b
            elif c.mode == "xenia":                  # Xenia MAX_V128: maxps(a,b) | maxps(b,a)
                r = (np.where(a > b, a, b).view(np.uint32) | np.where(b > a, b, a).view(np.uint32)).view(np.float32)
            else:
                r = vmaxmin_isa(a, b, True)
        elif opname == "min":
            if c.mode == "recomp":
                r = np.where(a < b, a, b)
            elif c.mode == "xenia":                  # Xenia MIN_V128: minps(a,b) | minps(b,a)
                r = (np.where(a < b, a, b).view(np.uint32) | np.where(b < a, b, a).view(np.uint32)).view(np.float32)
            else:
                r = vmaxmin_isa(a, b, False)
        return flush_denorm(np.asarray(r, dtype=np.float32))
    return g


def vmaxmin_isa(a, b, is_max):
    out = np.empty(4, dtype=np.float32)
    for i in range(4):
        x, y = a[i], b[i]
        if np.isnan(x):
            out[i] = np.float32(np.nan); continue
        if np.isnan(y):
            out[i] = np.float32(np.nan); continue
        if x == y == 0:
            sx, sy = np.signbit(x), np.signbit(y)
            if is_max:
                out[i] = x if not sx else y
            else:
                out[i] = x if sx else y
            continue
        out[i] = (x if x > y else y) if is_max else (x if x < y else y)
    return out


def vmadd(c, a_r, c_r, b_r, negsub=False):
    """vmaddfp: a*c + b ; vnmsubfp: -(a*c - b). Inputs flushed (Xenia VectorDenormFlush)."""
    a = flush_denorm(c.vf(a_r)); cc = flush_denorm(c.vf(c_r)); b = flush_denorm(c.vf(b_r))
    if c.mode == "recomp":
        p = (a * cc).astype(np.float32)
        p = flush_denorm(p)
        r = (p - b) if negsub else (p + b)
        r = -r if negsub else r
        return flush_denorm(np.asarray(r, dtype=np.float32))
    if negsub:
        r = fused_f32_vec(a, cc, -b)
        r = -r
    else:
        r = fused_f32_vec(a, cc, b)
    return flush_denorm(np.asarray(r, dtype=np.float32))


def vdot(c, va, vb, n):
    a = c.vf(va); b = c.vf(vb)
    if c.mode == "recomp":
        # SSE4.1 dpps (DAZ/FTZ): products rounded to single; ((p0+p1)+(p2+p3)) on HOST lanes.
        # recomp keeps guest element j in host lane 3-j, so host (0,1,2,3) = guest (3,2,1,0).
        af = flush_denorm(a); bf = flush_denorm(b)
        p = [np.float32(af[j] * bf[j]) if j < n else np.float32(0) for j in range(4)]
        p = [flush_denorm(np.array([x], dtype=np.float32))[0] for x in p]
        # The guest lib is built for baseline x86-64 (no -msse4.1), so simde_mm_dp_ps takes its
        # PORTABLE path: sum = 0; sum += h0; sum += h1; sum += h2; sum += h3 (host lanes, each
        # add rounded to single under the flush-mode MXCSR).
        h = [p[3], p[2], p[1], p[0]]
        s = np.float32(0.0)
        for x in h:
            s = flush_denorm(np.array([np.float32(s + x)], dtype=np.float32))[0]
        return flush_denorm(np.array([s] * 4, dtype=np.float32))
    # Xenia DOT_PRODUCT_3/4 (x64_sequences.cc): inputs flushed (Vmx MXCSR DAZ), products and sums
    # in double, dp3 = (p0 + p2) + p1, dp4 = (p0 + p2) + (p1 + p3); one rounding to single;
    # overflow -> QNaN; output denormals flushed.
    af = flush_denorm(a); bf = flush_denorm(b)
    p = [float(af[j]) * float(bf[j]) for j in range(4)]
    if n == 3:
        s = (p[0] + p[2]) + p[1]
    else:
        s = (p[0] + p[2]) + (p[1] + p[3])
    r = np.float32(s)
    if np.isinf(r) and not any(np.isinf(x) for x in list(af[:n]) + list(bf[:n])):
        r = np.float32(np.nan)
    elif np.isinf(r):
        r = np.float32(np.nan)
    return flush_denorm(np.array([r] * 4, dtype=np.float32))


def vx128_regs(w):
    vd = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
    va = ((w >> 16) & 31) | (((w >> 5) & 1) << 5) | (((w >> 10) & 1) << 6)
    vb = ((w >> 11) & 31) | ((w & 3) << 5)
    return vd, va, vb


def vx128_1_vd(w):
    return ((w >> 21) & 31) | (((w >> 2) & 3) << 5)


def vx128_3(w):
    vd = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
    vb = ((w >> 11) & 31) | ((w & 3) << 5)
    imm = (w >> 16) & 31
    return vd, vb, imm


def _vfloat_unop(name, fn):
    return fn, name


def v_ctsxs(c, vd, vb, uimm):
    x = c.vf(vb).astype(np.float64) * (2.0 ** uimm)
    out = []
    for v in x:
        if np.isnan(v):
            out.append(0)
        else:
            t = math.trunc(v) if not math.isinf(v) else (1 << 40 if v > 0 else -(1 << 40))
            t = max(-0x80000000, min(0x7FFFFFFF, t))
            out.append(t & M32)
    c.vr[vd] = out


def v_cfsx(c, vd, vb, uimm):
    ints = [sx32(x) for x in c.vr[vb]]
    r = np.array([np.float32(i) for i in ints], dtype=np.float32)   # RNE int->float
    if uimm:
        r = (r * np.float32(2.0 ** -uimm)).astype(np.float32)
    c.set_vf(vd, flush_denorm(r))


def v_cmp(c, vd, va, vb, kind, rc):
    a = flush_denorm(c.vf(va)); b = flush_denorm(c.vf(vb))
    if kind == "ge":
        m = a >= b
    elif kind == "gt":
        m = a > b
    elif kind == "eq":
        m = a == b
    res = [M32 if x else 0 for x in m]
    c.vr[vd] = res
    if rc:
        allt = all(res); nonet = not any(res)
        c.cr[6] = (8 if allt else 0) | (2 if nonet else 0)


def v_refp(c, vd, vb):
    b = flush_denorm(c.vf(vb))
    with np.errstate(all="ignore"):
        r = (np.float32(1.0) / b).astype(np.float32)
    c.set_vf(vd, flush_denorm(r))


# vrsqrtefp: Xenia (x64_backend.cc EmitScalarVRsqrteHelper, per lane via the vector helper) emulates
# the hardware estimate with a 32-entry table; XenonRecomp lowers it to 1/sqrt (simde sqrt_ps then
# div_ps: two correctly rounded single ops).
_VRSQRTE_TABLE = [0x568B4FD, 0x4F3AF97, 0x48DAAA5, 0x435A618, 0x3E7A1E4, 0x3A29DFE, 0x3659A5C, 0x32E96F8,
                  0x2FC93CA, 0x2D090CE, 0x2A88DFE, 0x2838B57, 0x26188D4, 0x2438673, 0x2268431, 0x20B820B,
                  0x3D27FFA, 0x3807C29, 0x33878AA, 0x2F97572, 0x2C27279, 0x2926FB7, 0x2666D26, 0x23F6AC0,
                  0x21D6881, 0x1FD6665, 0x1E16468, 0x1C76287, 0x1AF60C1, 0x1995F12, 0x1855D79, 0x1735BF4]


def _lzcnt32(x):
    x &= M32
    return 32 - x.bit_length()


def xenia_vrsqrte_scalar(u):
    """port of Xenia's scalar vrsqrtefp helper (VMX NJM on: denormal inputs act as signed zero)."""
    u &= M32
    mant = u & 0x7FFFFF
    exp = (u >> 23) & 0xFF
    neg = u >> 31
    if u == 0xFF800000:
        return 0x7FC00000
    if exp == 0:
        return 0xFF800000 if neg else 0x7F800000         # +-0 and (NJM) denormals -> +-inf
    if exp == 255:
        if mant:
            return u | 0x400000                            # quiet the NaN
        return 0x7FC00000 if neg else 0                    # +inf -> +0 (-inf handled above)
    if neg:
        return 0x7FC00000
    eax = ((exp - 127) << 4) & 16
    ecx = (127 - exp) >> 1                                 # sar (python >> is arithmetic)
    eax = (eax | (mant >> 19)) ^ 16
    t = _VRSQRTE_TABLE[eax]
    edx = ((mant >> 9) & 1023) * (t >> 16) & M32
    eax = (((t << 10) & 0x3FFFC00) - edx) & M32
    if not (eax >> 25) & 1:
        ecx += 6
        lz = _lzcnt32(eax & 0x1FFFFFF)
        sh = (lz - 6) & 31
        ecx -= lz
        eax = (eax << sh) & M32
    if (eax & 5) and (eax & 2):
        eax = (eax + 4) & M32
    r = (((ecx << 23) + 0x3F800000) & M32) | (u & 0x80000000) | ((eax >> 2) & 0x7FFFFF)
    if (r & 0x7F800000) == 0:                              # vaddss +0 under VMX DAZ/FTZ
        r &= 0x80000000
    return r


def v_rsqrte(c, vd, vb):
    if c.mode == "recomp":
        b = flush_denorm(c.vf(vb))
        with np.errstate(all="ignore"):
            r = (np.float32(1.0) / np.sqrt(b)).astype(np.float32)
        c.set_vf(vd, flush_denorm(r))
    else:
        c.vr[vd] = [xenia_vrsqrte_scalar(x) for x in c.vr[vb]]


# ---- VMX byte / halfword element helpers (element 0 = most significant, big-endian) --------
def _h8(c, r):
    out = []
    for w in c.vr[r]:
        out += [(w >> 16) & 0xFFFF, w & 0xFFFF]
    return out


def _set_h8(c, r, hs):
    c.vr[r] = [((hs[2 * i] & 0xFFFF) << 16) | (hs[2 * i + 1] & 0xFFFF) for i in range(4)]


def _b16(c, r):
    return list(_vbytes(c, r))


def _set_b16(c, r, bs):
    _set_vbytes(c, r, bytes(b & 0xFF for b in bs))


def _sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def _cr6_from(c, res):
    """Rc=1 vector compare: CR6 = all_true | 0 | none_true | 0 (Power ISA 2.x, VXR form)."""
    c.cr[6] = (8 if all(res) else 0) | (2 if not any(res) else 0)


# iter-463: integer vector compares with their record forms (the vcmpgtuh. CR6 audit), plus the
# halfword arithmetic the VC-1 decoder uses. Power ISA AltiVec definitions, element-wise.
_VCMP_INT = {  # base xo11 -> (name, element bits, predicate on (a, b) raw unsigned elements)
    6: ("vcmpequb", 8, lambda a, b: a == b),
    70: ("vcmpequh", 16, lambda a, b: a == b),
    518: ("vcmpgtub", 8, lambda a, b: a > b),
    582: ("vcmpgtuh", 16, lambda a, b: a > b),
    646: ("vcmpgtuw", 32, lambda a, b: a > b),
    774: ("vcmpgtsb", 8, lambda a, b: _sx(a, 8) > _sx(b, 8)),
    838: ("vcmpgtsh", 16, lambda a, b: _sx(a, 16) > _sx(b, 16)),
    902: ("vcmpgtsw", 32, lambda a, b: _sx(a, 32) > _sx(b, 32)),
}


def dec_op4_int(w, vd, va, vb, xo11):
    base = xo11 & 0x3FF
    if base in _VCMP_INT and (xo11 & ~0x400) == base:
        name, bits, pred = _VCMP_INT[base]
        rc = (w >> 10) & 1
        get, put, ones = {8: (_b16, _set_b16, 0xFF), 16: (_h8, _set_h8, 0xFFFF),
                          32: (lambda c, r: list(c.vr[r]), lambda c, r, v: c.vr.__setitem__(r, list(v)), M32)}[bits]
        def f(c):
            res = [ones if pred(a, b) else 0 for a, b in zip(get(c, va), get(c, vb))]
            put(c, vd, res)
            if rc:
                _cr6_from(c, res)
        return f, name + ("." if rc else "")
    if xo11 == 64:
        def f(c): _set_h8(c, vd, [(a + b) & 0xFFFF for a, b in zip(_h8(c, va), _h8(c, vb))])
        return f, "vadduhm"
    if xo11 == 1856:
        def f(c): _set_h8(c, vd, [max(-32768, min(32767, _sx(a, 16) - _sx(b, 16))) for a, b in zip(_h8(c, va), _h8(c, vb))])
        return f, "vsubshs"
    if xo11 == 324:
        def f(c): _set_h8(c, vd, [(a << (b & 15)) & 0xFFFF for a, b in zip(_h8(c, va), _h8(c, vb))])
        return f, "vslh"
    if xo11 == 836:
        def f(c): _set_h8(c, vd, [_sx(a, 16) >> (b & 15) for a, b in zip(_h8(c, va), _h8(c, vb))])
        return f, "vsrah"
    if xo11 == 780:
        simm = va - 32 if va & 16 else va
        def f(c): _set_b16(c, vd, [simm] * 16)
        return f, "vspltisb"
    if xo11 == 844:
        simm = va - 32 if va & 16 else va
        def f(c): _set_h8(c, vd, [simm] * 8)
        return f, "vspltish"
    return None, None


def dec_op4(w, pc):
    vd, va, vb = fields(w)
    # VMX128 memory forms
    key128 = (((w >> 4) & 0x7F) << 4) | (w & 3)
    mem128 = {0b00011000011: "lvx128", 0b00111000011: "stvx128", 0b10000000011: "lvlx128",
              0b10001000011: "lvrx128", 0b10100000011: "stvlx128", 0b10101000011: "stvrx128",
              0b00110000011: "stvewx128", 0b01011000011: "lvxl128", 0b01111000011: "stvxl128",
              0b00010000011: "lvewx128", 0b00000000011: "lvsl128", 0b00001000011: "lvsr128"}
    if key128 in mem128:
        name = mem128[key128]
        vd128 = vx128_1_vd(w)
        ra, rb = (w >> 16) & 31, (w >> 11) & 31
        def eax(c):
            return ((c.gpr[ra] if ra else 0) + c.gpr[rb]) & M32
        if name in ("lvx128", "lvxl128"):
            def f(c): c.vr[vd128] = list(struct.unpack(">4I", c.mem.read_bytes(eax(c) & ~0xF, 16)))
        elif name in ("stvx128", "stvxl128"):
            def f(c): c.mem.write_bytes(eax(c) & ~0xF, struct.pack(">4I", *c.vr[vd128]))
        elif name == "lvlx128":
            def f(c): lvlx(c, vd128, eax(c))
        elif name == "lvrx128":
            def f(c): lvrx(c, vd128, eax(c))
        elif name == "stvlx128":
            def f(c): stvlx(c, vd128, eax(c))
        elif name == "stvrx128":
            def f(c): stvrx(c, vd128, eax(c))
        elif name == "stvewx128":
            def f(c): stvewx(c, vd128, eax(c))
        elif name == "lvewx128":
            def f(c): lvewx(c, vd128, eax(c))
        else:
            return None, name
        return f, name
    xo6 = w & 0x3F
    if xo6 in (46, 47):   # vmaddfp / vnmsubfp  (VA-form: vD, vA, vB, vC)
        vc = (w >> 6) & 31
        if xo6 == 46:
            def f(c): c.set_vf(vd, vmadd(c, va, vc, vb))
            return f, "vmaddfp"
        def f(c): c.set_vf(vd, vmadd(c, va, vc, vb, negsub=True))
        return f, "vnmsubfp"
    if xo6 == 42:   # vsel
        vc = (w >> 6) & 31
        def f(c):
            c.vr[vd] = [(b & m) | (a & ~m & M32) for a, b, m in zip(c.vr[va], c.vr[vb], c.vr[vc])]
        return f, "vsel"
    if xo6 == 43:   # vperm
        vc = (w >> 6) & 31
        def f(c):
            src = _vbytes(c, va) + _vbytes(c, vb)
            ctl = _vbytes(c, vc)
            _set_vbytes(c, vd, bytes(src[x & 31] for x in ctl))
        return f, "vperm"
    if xo6 == 44:   # vsldoi
        sh = (w >> 6) & 15
        def f(c):
            src = _vbytes(c, va) + _vbytes(c, vb)
            _set_vbytes(c, vd, src[sh:sh + 16])
        return f, "vsldoi"
    xo11 = w & 0x7FF
    fb = {10: "add", 74: "sub", 1034: "max", 1098: "min"}
    if xo11 in fb:
        g = vec_bin_float(fb[xo11])
        def f(c): c.set_vf(vd, g(c, va, vb))
        return f, "v%sfp" % fb[xo11]
    if xo11 == 1028:
        def f(c): c.vr[vd] = [a & b for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vand"
    if xo11 == 1092:
        def f(c): c.vr[vd] = [a & ~b & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vandc"
    if xo11 == 1156:
        def f(c): c.vr[vd] = [a | b for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vor"
    if xo11 == 1220:
        def f(c): c.vr[vd] = [a ^ b for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vxor"
    if xo11 == 1284:
        def f(c): c.vr[vd] = [~(a | b) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vnor"
    if xo11 == 652:
        idx = va & 3
        def f(c):
            x = c.vr[vb][idx]
            c.vr[vd] = [x, x, x, x]
        return f, "vspltw"
    if xo11 == 908:
        simm = va - 32 if va & 16 else va
        def f(c): c.vr[vd] = [simm & M32] * 4
        return f, "vspltisw"
    if xo11 == 140:
        def f(c):
            A, B = c.vr[va], c.vr[vb]
            c.vr[vd] = [A[0], B[0], A[1], B[1]]
        return f, "vmrghw"
    if xo11 == 396:
        def f(c):
            A, B = c.vr[va], c.vr[vb]
            c.vr[vd] = [A[2], B[2], A[3], B[3]]
        return f, "vmrglw"
    if xo11 == 644:
        def f(c): c.vr[vd] = [a >> (b & 31) for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vsrw"
    if xo11 == 388:
        def f(c): c.vr[vd] = [(a << (b & 31)) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vslw"
    if xo11 == 900:
        def f(c): c.vr[vd] = [(sx32(a) >> (b & 31)) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vsraw"
    if xo11 == 128:
        def f(c): c.vr[vd] = [(a + b) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vadduwm"
    if xo11 == 1152:
        def f(c): c.vr[vd] = [(a - b) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vsubuwm"
    if xo11 == 266:
        def f(c): v_refp(c, vd, vb)
        return f, "vrefp"
    if xo11 == 330:
        def f(c): v_rsqrte(c, vd, vb)
        return f, "vrsqrtefp"
    if xo11 in (522, 586, 650, 714):   # vrfin vrfiz vrfip vrfim
        kind = {522: "n", 586: "z", 650: "p", 714: "m"}[xo11]
        def f(c):
            x = flush_denorm(c.vf(vb))
            out = []
            for v in x:
                v = float(v)
                if math.isnan(v) or math.isinf(v):
                    out.append(v); continue
                if kind == "z":
                    r = float(math.trunc(v))
                elif kind == "p":
                    r = float(math.ceil(v))
                elif kind == "m":
                    r = float(math.floor(v))
                elif c.mode == "recomp" and PRE463_ROUNDEVEN:   # a guest lib built BEFORE the iter-463
                                                                  # roundevenf fix (tie direction bug)
                    r = float(np.float32(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1))
                    d = v - r
                    if abs(d) == 0.5 and math.fmod(r, 2.0) != 0.0:
                        r = r - 1.0 if d > 0 else r + 1.0
                else:                      # Xenia: Round(ROUND_TO_NEAREST) = vroundps RNE; since iter-463
                                           # also recomp mode (math_helpers.cpp roundevenf == ROUNDPS,
                                           # proven over all 2^32 inputs by difftest --fp-roundeven-exhaustive)
                    r = float(round(v))
                out.append(math.copysign(r, v) if r == 0 else r)
            c.set_vf(vd, np.array(out, dtype=np.float32))
        return f, "vrfi" + kind
    if xo11 == 970:
        def f(c): v_ctsxs(c, vd, vb, va)
        return f, "vctsxs"
    if xo11 == 842:
        def f(c): v_cfsx(c, vd, vb, va)
        return f, "vcfsx"
    cmps = {454: "ge", 710: "gt", 198: "eq"}
    base = xo11 & 0x3FF
    if base in cmps:
        rc = (w >> 10) & 1
        kind = cmps[base]
        def f(c): v_cmp(c, vd, va, vb, kind, rc)
        return f, "vcmp%sfp%s" % (kind, "." if rc else "")
    if xo11 == 134 or xo11 == 134 + 1024:
        rc = (w >> 10) & 1
        def f(c):
            res = [M32 if a == b else 0 for a, b in zip(c.vr[va], c.vr[vb])]
            c.vr[vd] = res
            if rc:
                c.cr[6] = (8 if all(res) else 0) | (2 if not any(res) else 0)
        return f, "vcmpequw"
    f, name = dec_op4_int(w, vd, va, vb, xo11)
    if f is not None:
        return f, name
    return None, "op4/xo11=%d" % xo11


def dec_op5(w, pc):
    vd, va, vb = vx128_regs(w)
    if ((w >> 9) & 1) == 0 and ((w >> 4) & 1) == 0:   # vperm128
        vc = (w >> 6) & 7
        def f(c):
            src = _vbytes(c, va) + _vbytes(c, vb)
            ctl = _vbytes(c, vc)
            _set_vbytes(c, vd, bytes(src[x & 31] for x in ctl))
        return f, "vperm128"
    key = (((w >> 6) & 0xF) << 2) | ((w >> 4) & 1)
    if key in (1, 5, 9):
        g = vec_bin_float({1: "add", 5: "sub", 9: "mul"}[key])
        def f(c): c.set_vf(vd, g(c, va, vb))
        return f, {1: "vaddfp128", 5: "vsubfp128", 9: "vmulfp128"}[key]
    if key == 13:   # vmaddfp128: vD = vA*vB + vD
        def f(c): c.set_vf(vd, vmadd(c, va, vb, vd))
        return f, "vmaddfp128"
    if key == 17:   # vmaddcfp128: vD = vA*vD + vB
        def f(c): c.set_vf(vd, vmadd(c, va, vd, vb))
        return f, "vmaddcfp128"
    if key == 21:   # vnmsubfp128: vD = -(vA*vB - vD)
        def f(c): c.set_vf(vd, vmadd(c, va, vb, vd, negsub=True))
        return f, "vnmsubfp128"
    if key == 25:
        def f(c): c.set_vf(vd, vdot(c, va, vb, 3))
        return f, "vmsum3fp128"
    if key == 29:
        def f(c): c.set_vf(vd, vdot(c, va, vb, 4))
        return f, "vmsum4fp128"
    logic = {33: "and", 37: "andc", 41: "nor", 45: "or", 49: "xor"}
    if key in logic:
        k = logic[key]
        def f(c):
            A, B = c.vr[va], c.vr[vb]
            if k == "and":
                c.vr[vd] = [a & b for a, b in zip(A, B)]
            elif k == "andc":
                c.vr[vd] = [a & ~b & M32 for a, b in zip(A, B)]
            elif k == "nor":
                c.vr[vd] = [~(a | b) & M32 for a, b in zip(A, B)]
            elif k == "or":
                c.vr[vd] = [a | b for a, b in zip(A, B)]
            else:
                c.vr[vd] = [a ^ b for a, b in zip(A, B)]
        return f, "v%s128" % k
    if key == 53:   # vsel128: vD = (vA & ~vD) | (vB & vD)
        def f(c):
            A, B, D = c.vr[va], c.vr[vb], c.vr[vd]
            c.vr[vd] = [(b & m) | (a & ~m & M32) for a, b, m in zip(A, B, D)]
        return f, "vsel128"
    return None, "op5/key=%d" % key


# vpkd3d128 (iter-467 p2): Xenia ppc_emit_altivec.cc InstrEmit_vpkd3d128 (type / pack / shift and the
# MakePermuteMask table) + x64_seq_vector.cc PACK (EmitD3DCOLOR, EmulateFLOAT16_2,
# EmulateFLOAT16_4_RoundToEven) + base/math.h float_to_xenos_half. VX128_4 fields: VD128l 21-25, IMM 16-20,
# VB128l 11-15, z 6-7, VD128h 2-3, VB128h 0-1. Element order = PPC (element 0 = x), as everywhere here.
# recomp mode = XenonRecomp's emitted conventions where it has them (D3DCOLOR: no NaN max-first; FLOAT16_4:
# truncate, denormals shifted, > 65504 -> 0x7FFF); type 3 has no XenonRecomp form (the pristine output traps)
# so every mode follows Xenia.
def xenos_half(u, preserve_denormal=False, rne=False):
    u &= M32
    a = u & 0x7FFFFFFF
    if a >= 0x47FFE000:
        r = 0x7FFF
    else:
        if a < 0x38800000:
            if preserve_denormal:
                sh = min(113 - (a >> 23), 24)
                r = (0x800000 | (a & 0x7FFFFF)) >> sh
            else:
                r = 0
        else:
            r = (a + 0xC8000000) & M32
        if rne:
            r = (r + 0xFFF + ((r >> 13) & 1)) & M32
        r = (r >> 13) & 0x7FFF
    return r | ((u & 0x80000000) >> 16)


def xr_half_f16_4(u):   # XenonRecomp's FLOAT16_4 lane (the deployed sub_8210C090 type-5 site)
    u &= M32
    t = bits_f32(u & 0x7FFFFFFF)
    e = 0xFF if (t != t or t > 65504.0) else (u & 0x7F800000) >> 23
    m = (u & 0x7FE000) >> 13 if e != 0xFF else 0
    if e == 0xFF:
        h = 0x7FFF
    elif e > 0x70:
        h = (((e - 0x70) << 10) + m) & 0xFFFF
    else:
        h = 0 if (0x71 - e) > 31 else ((0x400 + m) >> (0x71 - e)) & 0xFFFF
    return h | ((u & 0x80000000) >> 16)


def _d3dcolor_lane(u, mode):
    f = bits_f32(u)
    lo, hi = 3.0, bits_f32(0x404000FF)
    if mode == "recomp":
        f = lo if f < lo else (hi if f > hi else f)          # NaN passes through
    else:
        f = lo if (f != f or f < lo) else f                    # x86 maxps(src, 3.0): NaN -> 3.0
        f = hi if f > hi else f
    return f32_bits(f) & 0xFF


def _perm_mask(pack, shift):
    tab = {(1, 0): ((0, 0), (0, 1), (0, 2), (1, 3)), (1, 1): ((0, 0), (0, 1), (1, 3), (0, 3)),
           (1, 2): ((0, 0), (1, 3), (0, 2), (0, 3)), (1, 3): ((1, 3), (0, 1), (0, 2), (0, 3)),
           (2, 0): ((0, 0), (0, 1), (1, 2), (1, 3)), (2, 1): ((0, 0), (1, 2), (1, 3), (0, 3)),
           (2, 2): ((1, 2), (1, 3), (0, 2), (0, 3)), (2, 3): ((1, 3), (0, 1), (0, 2), (0, 3)),
           (3, 0): ((0, 0), (0, 1), (1, 2), (1, 3)), (3, 1): ((0, 0), (1, 2), (1, 3), (0, 3)),
           (3, 2): ((1, 2), (1, 3), (0, 2), (0, 3)), (3, 3): ((0, 0), (0, 1), (0, 2), (1, 2))}
    return tab.get((pack, shift))


def dec_vpkd3d128(w):
    vd = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
    vb = ((w >> 11) & 31) | ((w & 3) << 5)
    imm = (w >> 16) & 31
    typ, pack, shift = imm >> 2, imm & 3, (w >> 6) & 3
    perm = _perm_mask(pack, shift)
    if typ not in (0, 3, 5) or perm is None:
        return None, "vpkd3d128 type %d pack %d shift %d" % (typ, pack, shift)

    def f(c):
        B = c.vr[vb]
        if typ == 0:     # D3DCOLOR: element 3 = ARGB of (x, y, z, w) low bytes
            x, y, z, ww = (_d3dcolor_lane(B[i], c.mode) for i in range(4))
            packed = [0, 0, 0, ((ww << 24) | (x << 16) | (y << 8) | z) & M32]
        elif typ == 3:   # FLOAT16_2: element 3 = half(x) << 16 | half(y)
            packed = [0, 0, 0, ((xenos_half(B[0]) << 16) | xenos_half(B[1])) & M32]
        else:            # FLOAT16_4: element 2 = half(x) << 16 | half(y), element 3 = half(z) << 16 | half(w)
            h = (lambda u: xr_half_f16_4(u)) if c.mode == "recomp" else (lambda u: xenos_half(u, False, True))
            packed = [0, 0, ((h(B[0]) << 16) | h(B[1])) & M32, ((h(B[2]) << 16) | h(B[3])) & M32]
        prev = list(c.vr[vd])
        c.vr[vd] = [(packed if s else prev)[i] for s, i in perm]
    return f, "vpkd3d128.t%d" % typ


def dec_op6(w, pc):
    vd, va, vb = vx128_regs(w)
    if ((w >> 9) & 3) == 1 and ((w >> 4) & 3) == 1:   # vpermwi128
        vd_p = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
        vb_p = ((w >> 11) & 31) | ((w & 3) << 5)
        perm = ((w >> 16) & 31) | (((w >> 6) & 7) << 5)
        sel = [(perm >> 6) & 3, (perm >> 4) & 3, (perm >> 2) & 3, perm & 3]
        def f(c):
            B = c.vr[vb_p]
            c.vr[vd_p] = [B[sel[0]], B[sel[1]], B[sel[2]], B[sel[3]]]
        return f, "vpermwi128"
    if ((((w >> 8) & 7) << 4) | ((w >> 4) & 3)) == 0b1110001:   # vrlimi128 (Xenia ppc_emit_altivec.cc)
        vd4 = ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
        vb4 = ((w >> 11) & 31) | ((w & 3) << 5)
        imm4 = (w >> 16) & 31
        rot = (w >> 6) & 3
        def f(c):
            B, D = c.vr[vb4], c.vr[vd4]
            r = [B[(n + rot) & 3] for n in range(4)]
            c.vr[vd4] = [r[n] if (imm4 >> (3 - n)) & 1 else D[n] for n in range(4)]
        return f, "vrlimi128"
    if ((((w >> 8) & 7) << 4) | ((w >> 4) & 3)) == 0b1100001:   # vpkd3d128 (iter-467 p2)
        return dec_vpkd3d128(w)
    key3 = (w >> 4) & 0x7F
    vd3, vb3, imm = vx128_3(w)
    if key3 == 0b1111111:   # vupkd3d128 (Xenia x64_seq_vector.cc UNPACK)
        typ = imm >> 2
        def s16(x):
            x &= 0xFFFF
            return x - 0x10000 if x & 0x8000 else x
        def fix(u):   # Xenia: negative overflow (0x403F8000) -> QNaN; XenonRecomp has no such check
            u &= M32
            return 0x7FC00000 if (u == 0x403F8000 and MODE[0] != "recomp") else u
        if typ == 1:        # NORMSHORT2: (3 + hi(w)*2^-22, 3 + lo(w)*2^-22, 0, 1)
            def f(c):
                MODE[0] = c.mode
                v = c.vr[vb3][3]
                c.vr[vd3] = [fix(0x40400000 + s16(v >> 16)), fix(0x40400000 + s16(v)), 0, 0x3F800000]
            return f, "vupkd3d128.short2"
        if typ == 4:        # NORMSHORT4: (hi(z), lo(z), hi(w), lo(w)) each 3 + v*2^-22
            def f(c):
                MODE[0] = c.mode
                z, ww = c.vr[vb3][2], c.vr[vb3][3]
                c.vr[vd3] = [fix(0x40400000 + s16(z >> 16)), fix(0x40400000 + s16(z)),
                             fix(0x40400000 + s16(ww >> 16)), fix(0x40400000 + s16(ww))]
            return f, "vupkd3d128.short4"
        return None, "vupkd3d128 type %d" % typ
    if key3 == 0b1110011:
        idx = imm & 3
        def f(c):
            x = c.vr[vb3][idx]
            c.vr[vd3] = [x, x, x, x]
        return f, "vspltw128"
    if key3 == 0b1100011:
        def f(c): v_refp(c, vd3, vb3)
        return f, "vrefp128"
    if key3 == 0b1100111:
        def f(c): v_rsqrte(c, vd3, vb3)
        return f, "vrsqrtefp128"
    if key3 == 0b0101011:
        def f(c): v_cfsx(c, vd3, vb3, imm)
        return f, "vcsxwfp128"
    if key3 == 0b0100011:
        def f(c): v_ctsxs(c, vd3, vb3, imm)
        return f, "vcfpsxws128"
    if key3 == 0b1110111:
        simm = imm - 32 if imm & 16 else imm
        def f(c): c.vr[vd3] = [simm & M32] * 4
        return f, "vspltisw128"
    key4 = (((w >> 7) & 7) << 3) | ((w >> 4) & 1)
    cmpk = {0: "eq", 8: "ge", 16: "gt"}
    if key4 in cmpk:
        rc = (w >> 6) & 1
        kind = cmpk[key4]
        def f(c): v_cmp(c, vd, va, vb, kind, rc)
        return f, "vcmp%sfp128%s" % (kind, "." if rc else "")
    key2 = (((w >> 6) & 0xF) << 2) | ((w >> 4) & 1)
    if key2 in (40, 44):
        g = vec_bin_float("max" if key2 == 40 else "min")
        def f(c): c.set_vf(vd, g(c, va, vb))
        return f, "vmaxfp128" if key2 == 40 else "vminfp128"
    if key2 == 48:
        def f(c):
            A, B = c.vr[va], c.vr[vb]
            c.vr[vd] = [A[0], B[0], A[1], B[1]]
        return f, "vmrghw128"
    if key2 == 52:
        def f(c):
            A, B = c.vr[va], c.vr[vb]
            c.vr[vd] = [A[2], B[2], A[3], B[3]]
        return f, "vmrglw128"
    if key2 == 29:
        def f(c): c.vr[vd] = [a >> (b & 31) for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vsrw128"
    if key2 == 13:
        def f(c): c.vr[vd] = [(a << (b & 31)) & M32 for a, b in zip(c.vr[va], c.vr[vb])]
        return f, "vslw128"
    return None, "op6/%08X" % w


def dec_sc(w, pc):
    return None, "sc"


def dec_twi(w, pc):
    def f(c):
        c.log.append("twi at %08X (ignored)" % pc)
    return f, "twi"


for _op, _d in {
    3: dec_twi, 4: dec_op4, 5: dec_op5, 6: dec_op6, 7: dec_mulli, 8: dec_subfic, 10: dec_cmpli, 11: dec_cmpi,
    12: dec_addic, 13: lambda w, pc: dec_addic(w, pc, True), 14: dec_addi, 15: dec_addis,
    16: dec_bc, 17: dec_sc, 18: dec_b, 19: dec_op19, 20: dec_rlwimi, 21: dec_rlwinm, 23: dec_rlwnm,
    24: dec_logic_imm("ori"), 25: dec_logic_imm("oris"), 26: dec_logic_imm("xori"),
    27: dec_logic_imm("xoris"), 28: dec_logic_imm("andi."), 29: dec_logic_imm("andis."),
    30: dec_op30, 31: dec_op31,
    32: dec_load("lwz"), 33: dec_load("lwz", True), 34: dec_load("lbz"), 35: dec_load("lbz", True),
    36: dec_load("stw"), 37: dec_load("stw", True), 38: dec_load("stb"), 39: dec_load("stb", True),
    40: dec_load("lhz"), 41: dec_load("lhz", True), 42: dec_load("lha"), 43: dec_load("lha", True),
    44: dec_load("sth"), 45: dec_load("sth", True), 46: dec_load("lmw"), 47: dec_load("stmw"),
    48: dec_load("lfs"), 49: dec_load("lfs", True), 50: dec_load("lfd"), 51: dec_load("lfd", True),
    52: dec_load("stfs"), 53: dec_load("stfs", True), 54: dec_load("stfd"), 55: dec_load("stfd", True),
    58: dec_op58, 59: dec_op59, 62: dec_op62, 63: dec_op63,
}.items():
    DECODERS[_op] = _d
