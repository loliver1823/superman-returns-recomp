#!/usr/bin/env python3
"""gen_vpk_f16_cases.py - iter-467 p2 L6 golden-call cases for the vpkd3d128 sites of sub_8210C090.

sub_8210C090 = SurfaceClearColorPackByFormat: r3 = EDRAM colour format (jump table byte_820BA2A0),
v1 = the clear colour (x, y, z, w), r5 / r6 = out words, returns 32 or 64 in r3.
  fmt 6 (k_16_16_FLOAT)       -> loc_8210C3A4: vpkd3d128 type 3 (FLOAT16_2), pack 1, shift 3.
  fmt 7 (k_16_16_16_16_FLOAT) -> loc_8210C3C8: vpkd3d128 type 5 (FLOAT16_4).
Lanes cover the float_to_xenos_half edges: signed zero, denormal floor (0x38800000), the
65504 / 65535.5 / 131008 range, 0x47FFE000 saturation, inf, NaN, and the truncation bit.

    python scripts/difftest/golden_call/gen_vpk_f16_cases.py [out.script]
    golden_call_harness.exe <blob> <out.script> <recomp.out>            (SR_SPLICE_VPK_F16=0 / =1)
    python scripts/difftest/golden_call/run_script.py <blob> <out.script> <oracle.out> --mode xenia

Result (iter-467 p2): fmt 6, gate ON vs Xenia 43/43, gate OFF 1/43 (the old splice body packed
D3DCOLOR). fmt 7 matches XenonRecomp's own FLOAT16_4 lowering 43/43 but Xenia's in only 17/43
(Xenia rounds to nearest even, flushes denormals and saturates only from 0x47FFE000); that is a
separate emitter item.
"""
import sys

L = [0x00000000, 0x80000000, 0x3F800000, 0x3F000000, 0x3E800000, 0x3DCCCCCD, 0x3EAAAA9F,
     0x3F7FFFEF, 0x40000000, 0xBF800000, 0xBF000000, 0x42C88000, 0x447A2CCD, 0x477FE000,
     0x477FEF00, 0x477FF000, 0x4788B800, 0x47FFDFF3, 0x47FFE000, 0x48435000, 0xC788B800,
     0x38800000, 0x387BA882, 0x37FBA882, 0x33D6BF95, 0xB3D6BF95, 0x000116C2, 0x800116C2,
     0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0x7FBFFFFF, 0x3F801000,
     0x3F801FFB, 0x3900F990, 0x4640E6B6]
FIXED = [(0x3F800000, 0, 0, 0x3F800000), (0, 0x3F800000, 0, 0x3F800000),
         (0x3F000000, 0x3F000000, 0x3F000000, 0x3F800000),
         (0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000), (0, 0, 0, 0),
         (0, 0, 0, 0x3F800000)]   # the production colour of the 0x5A0 soft-particle RT clears


def vectors():
    n = len(L)
    for i in range(n):
        yield (L[i], L[(7 * i + 3) % n], L[(5 * i + 1) % n], L[(3 * i + 2) % n])
    yield from FIXED


def main():
    out = []
    for fmt in (6, 7):
        for k, v in enumerate(vectors()):
            out.append('CASE fmt%d_%02d' % (fmt, k))
            out.append('CALL 8210C090 r3=%d r4=0 r5=70001000 r6=70001010 v1=%s'
                       % (fmt, ''.join('%08x' % x for x in v)))
            out.append('PR r3')
            out.append('D 70001000 4')
            out.append('D 70001010 4')
            out.append('END')
    text = '\n'.join(out) + '\n'
    if len(sys.argv) > 1:
        open(sys.argv[1], 'w', newline='\n').write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
