#!/usr/bin/env python3
"""gen_vcmp_cr6_cases.py - iter-463 L6 golden-call cases for the vcmpgtuh. CR6 codegen fix.

sub_828FEB00 (VC-1 decoder, 1 of the 6 vcmpgtuh. sites): r3 = coefficient buffer,
r4 = 0 / != 0 path select, v1 = per-halfword shift, v2 = bias. The final
`vcmpgtuh. v0,(OR of all outputs & 0xFF00),0` is read by `mfocrf r11,cr6` -> r3.
Cases drive CR6 all-true (cr6=8), all-false (cr6=2) and mixed (cr6=0), plus seeded random
buffers. Uses the CALL vN= / PV / PCR script commands.

    python scripts/difftest/golden_call/gen_vcmp_cr6_cases.py [out.script]
    python scripts/difftest/golden_call/compare_run.py <out.script> --mode xenia   (and recomp, isa)
Before the fix 16 of 19 cases diverge (recomp r3=0 cr6=2: the loop's stale cmplwi cr6);
after it 0 of 19 in all three modes.
"""
import sys
import random
random.seed(463)
BUF = 0x70001000
out = []
def h8(vals): return ''.join('%04X' % (v & 0xFFFF) for v in vals)
def case(name, r4, v1, v2, data=None):
    n = 16 + (r4 + 1) * 8 * 48 + 64
    out.append('CASE ' + name)
    out.append('W %08X %s' % (BUF, (data or bytes(n)).hex().upper()))
    out.append('CALL 828FEB00 r3=%X r4=%X v1=%s v2=%s' % (BUF, r4, h8(v1), h8(v2)))
    out.append('PR r3')
    out.append('PCR 6')
    out.append('PV v9')
    out.append('D %08X %X' % (BUF, n))
    out.append('END')
z = [0] * 8
case('allfalse r4=0', 0, z, [1] * 8)
case('alltrue r4=0', 0, z, [0x100] * 8)
case('mixed r4=0', 0, z, [0x100, 1] * 4)
case('onelane r4=0', 0, z, [0] * 7 + [0x8000])
case('allfalse r4=1', 1, z, [0x7F] * 8)
case('alltrue r4=1', 1, z, [0xFF00] * 8)
case('mixed r4=1', 1, z, [1, 1, 0x200, 1, 1, 1, 1, 0x300])
for k in range(6):
    r4 = k % 3
    n = 16 + (r4 + 1) * 8 * 48 + 64
    amp = [0x3F, 0x7FF, 0x7FFF][k % 3]
    data = b''.join(((random.randint(-amp, amp)) & 0xFFFF).to_bytes(2, 'big') for _ in range(n // 2))
    case('random%d r4=%d amp=%X' % (k, r4, amp), r4, [3] * 8 if k % 2 else [random.randint(0, 6) for _ in range(8)],
         [4] * 8 if k < 3 else [random.randint(0, 0x40) for _ in range(8)], data)
for k, amp in enumerate([30, 200, 400, 700, 1000, 3000]):
    r4 = k % 2
    n = 16 + (r4 + 1) * 8 * 48 + 64
    data = b''.join(random.randint(0, amp).to_bytes(2, 'big') for _ in range(n // 2))
    case('nonneg%d r4=%d amp=%d' % (k, r4, amp), r4, [3] * 8, [4] * 8, data)
open(sys.argv[1] if len(sys.argv) > 1 else 'archive/golden_call/vcmp_cr6.script', 'w').write('\n'.join(out) + '\n')
print(len([l for l in out if l.startswith('CASE')]), 'cases')
