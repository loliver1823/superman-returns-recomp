// Host port of ShaderAssembleVertexFetches_sub_82107A30 (contract: native_vs_patch.hpp).
//
// Ported instruction by instruction from the recompiled body (guest/ppc ppc_recomp.1.cpp
// __imp__sub_82107A30, 422 instructions; IDA decompile cross-checked). Register names are
// kept in comments so a reviewer can line the two up.
//
// The uninitialised stack halfword (lhz r8,80(r1) at 0x82107A90) is PROVABLY irrelevant:
// only its low nibble enters (clrlwi r9,r8,28), the per-entry 16-bit field r9 is used only
// through r8 = r9 | 14 (ori at 0x82107C58), which forces bits 1-3; bit 0 survives but r8 is
// used only as an AND mask on the u16 table at 0x820BA0C0 = {E000, 1C00, 0380, 0070, 0008,
// 000A, 0008, 000E} (IDA bytes), whose entries are all even - so bit 0 never reaches any
// stored word. The live differential also runs the port with garbage 0x0000 and 0xFFFF.

#include "native_vs_patch.hpp"

#include "../kernel/guest_context.hpp"

#include <cstring>

namespace sr::native::vspatch {

namespace {

constexpr uint32_t kSwizTable = 0x820BA0C0u;  // u16[8]
constexpr uint32_t kFmtBytes = 0x820BB138u;   // u8[64], bytes per vertex format

thread_local bool rdOk = true;
thread_local int t_fail = 0;
uint32_t rd32(uint32_t va) {
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p) {
    rdOk = false;
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}
uint32_t rd16(uint32_t va) {
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p) {
    rdOk = false;
    return 0;
  }
  return (uint32_t(p[0]) << 8) | p[1];
}
uint32_t rd8(uint32_t va) {
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p) {
    rdOk = false;
    return 0;
  }
  return p[0];
}

inline uint32_t rotl(uint32_t x, int n) {
  n &= 31;
  return n ? (x << n) | (x >> (32 - n)) : x;
}
// PowerPC mask MB..ME (IBM bit numbering, 0 = MSB), wrapping when MB > ME.
inline uint32_t ppcMask(int mb, int me) {
  const uint32_t m1 = 0xFFFFFFFFu >> mb;
  const uint32_t m2 = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (m1 & m2) : (m1 | m2);
}
inline uint32_t rlwinm(uint32_t s, int sh, int mb, int me) { return rotl(s, sh) & ppcMask(mb, me); }
inline uint32_t rlwimi(uint32_t a, uint32_t s, int sh, int mb, int me) {
  const uint32_t m = ppcMask(mb, me);
  return (rotl(s, sh) & m) | (a & ~m);
}
// The bits-18/19 fixup used before every fetch-constant compare (rlwimi rX,rY,25,12,13).
inline uint32_t keyWord(uint32_t w) { return rlwimi(w, w, 25, 12, 13); }
inline int32_t fetchOffset(uint32_t w2) { return int32_t(w2 << 1) >> 9; }  // rlwinm 1,0,30 + srawi 9

}  // namespace

int lastFailure() { return t_fail; }

uint32_t ucodeDestVA(uint32_t vs, uint32_t variant) {
  const uint32_t hdr = vs + rd32(vs + 8u * (variant + 112u)) + 872u;
  return rd32(vs + 32u) + rd32(hdr);
}

uint32_t programBytes(uint32_t vs, uint32_t variant) {
  const uint32_t hdr = vs + rd32(vs + 8u * (variant + 112u)) + 872u;
  return rd32(hdr + 4u);
}

bool assembleVertexFetches(uint32_t vs, uint32_t decl, const uint8_t strides[16], uint32_t variant,
                           uint8_t* out, size_t outBytes, int64_t outBias, uint16_t garbage) {
  rdOk = true;
  t_fail = 0;
  uint16_t swz[8];
  for (uint32_t k = 0; k < 8; k++) swz[k] = uint16_t(rd16(kSwizTable + 2u * k));
  const uint32_t r25 = rd32(decl + 24u);                          // element count
  const uint32_t oldBase = variant * 416u + vs + 68u;             // r9 = r11 + 68
  const uint32_t hdr = rd32(vs + 8u * (variant + 112u)) + vs + 872u;
  const uint32_t r26 = rd32(hdr + 28u);                           // vfetch entry count
  const uint32_t table = rlwinm(rd32(hdr + 24u) + 9u, 2, 0, 29) + hdr;  // r30
  if (!rdOk || r26 > 41u) {  // the guest's stack array holds 124 dwords
    t_fail = rdOk ? 2 : 1;
    return false;
  }
  uint32_t L[124];
  // ---------------------------------------------------------------- phase 1
  uint32_t r8 = garbage;
  for (uint32_t i = 0; i < r26; i++) {
    const uint32_t e = rd32(table + 4u * i);                      // r9 = *(r27)
    uint32_t r11 = rd32(oldBase + 12u * i);
    const uint32_t r29 = rd32(oldBase + 12u * i + 4u);
    const uint32_t r28 = rd32(oldBase + 12u * i + 8u);
    uint32_t elem = decl + 52u;                                   // r5
    uint32_t r10 = 0;
    if (r25 != 0) {
      const uint32_t r31 = rlwinm(e, 20, 28, 31);                 // usage
      for (; r10 < r25; r10++, elem += 12u) {
        if (rd8(elem + 9u) != r31) continue;
        if (rd8(elem + 10u) == rlwinm(e, 16, 28, 31)) break;      // usage index
      }
    }
    uint32_t r9, r5;
    if (r25 == 0 || r10 >= r25) {                                 // loc_82107B00: unmatched
      r9 = (r8 & 0xFu) | 37456u;
      r11 = rlwimi(r11, 95u, 20, 2, 11);
      r10 = (r29 & 0xBFC6CFFFu) | 0x60000u;
      r5 = (r28 & 0x80000000u) | strides[0];
    } else {                                                      // loc_82107B30: matched
      const uint32_t t = rd32(elem + 4u);
      r9 = r8 & 0xFu;
      uint32_t r31 = rlwinm(t, 16, 29, 31);
      uint32_t x = rlwinm(t, 22, 26, 28);
      const uint32_t r21 = rlwinm(t, 13, 29, 31);
      x |= r31;
      r31 = rlwinm(t, 0, 16, 21);
      x = rlwinm(x, 3, 0, 28);
      const uint32_t r20 = rlwinm(t, 0, 24, 25);
      x |= r21;
      x = rlwinm(x, 1, 0, 30);
      x |= r31;
      x = rlwinm(x, 3, 0, 28);
      r9 = (x | r9) & 0xFFFFu;
      if (r20 == 64u) {
        uint32_t v = r9 & 0xFFFFu;
        if ((v & 0xE000u) <= 0x6000u) r9 = ((v & 0xE000u) ^ 0x2000u) | (v & 0x1FFFu);
        v = r9 & 0xFFFFu;
        if ((v & 0x1C00u) <= 0xC00u) r9 = ((v & 0x1C00u) ^ 0x400u) | (v & 0xE3FFu);
        v = r9 & 0xFFFFu;
        if ((v & 0x380u) <= 0x180u) r9 = ((v & 0x380u) ^ 0x80u) | (v & 0xFC7Fu);
        v = r9 & 0xFFFFu;
        if ((v & 0x70u) <= 0x30u) r9 = ((v & 0x70u) ^ 0x10u) | (v & 0xFF8Fu);
      }
      // loc_82107BF4
      const uint32_t r21b = rlwinm(t, 12, 14, 19);
      const uint32_t stream = rd16(elem + 0u);
      uint32_t ra = rlwinm(t, 0, 22, 23);
      const uint32_t off = rd16(elem + 2u);
      ra = r21b | ra;
      ra = rlwinm(ra, 4, 0, 27);
      if (stream >= 16u) {  // the guest would read past dev+12256+15 - not handled
        t_fail = 1;
        return false;
      }
      const uint32_t stride = strides[stream];
      r10 = ra | (r29 & 0xBFC0CFFFu);
      const uint32_t slot = 95u - stream;                         // subfic r29,r8,95
      uint32_t r5b = rlwinm(off, 6, 1, 23);
      const uint32_t prod = slot * 0x01560056u;                   // mullw (the /3 trick)
      uint32_t r31m = prod;
      r5b |= stride;
      r31m = rlwimi(r31m, prod, 11, 8, 12);
      r5 = r5b | (r28 & 0x80000000u);
      r11 = rlwimi(r11, r31m, 1, 5, 11);
      r11 = rlwinm(r11, 0, 5, 1);
    }
    // loc_82107C50: the destination swizzle (dword 1) against the format's components.
    r9 &= 0xFFFFu;
    L[3 * i + 0] = r11;
    L[3 * i + 2] = r5;
    r8 = r9 | 14u;
    const uint32_t m = r8 & 0xFFFFu;
    uint32_t a = swz[(r10 >> 0) & 7u] & m;
    {
      uint32_t f = a >> 3;
      f |= a; f >>= 3; f |= a; f >>= 3; f |= a; f >>= 3;
      a = f | a;
    }
    r10 = rlwimi(r10, a, 31, 29, 31);
    const uint32_t s1 = r10;
    uint32_t b = swz[(s1 >> 3) & 7u] & m;
    {
      uint32_t f = b >> 3;
      const uint32_t r31 = rlwinm(b, 2, 0, 29);
      f |= b; f >>= 3; f |= b; f >>= 3;
      uint32_t g = f | b;
      g = rlwinm(g, 31, 1, 31);
      r10 = g | r31;
      r10 = rlwimi(r10, s1, 0, 29, 25);
    }
    const uint32_t s2 = r10;
    uint32_t c = swz[(s2 >> 6) & 7u] & m;
    uint32_t s3;
    {
      uint32_t f = c >> 3;
      uint32_t r31 = rlwinm(c, 3, 0, 28);
      f |= c; r31 |= c;
      f >>= 3; r31 = rlwinm(r31, 2, 0, 29);
      uint32_t g = f | c;
      g = rlwinm(g, 31, 1, 31);
      s3 = g | r31;
      s3 = rlwimi(s3, s2, 0, 26, 22);
    }
    uint32_t d = swz[(s3 >> 9) & 7u] & m;
    {
      uint32_t h = rlwinm(d, 3, 0, 28);
      uint32_t r31 = d >> 3;
      h |= d; r31 |= d;
      h = rlwinm(h, 3, 0, 28); r31 = rlwinm(r31, 31, 1, 31);
      uint32_t g = h | d;
      g = rlwinm(g, 2, 0, 29);
      g |= r31;
      g = rlwimi(g, s3, 0, 23, 19);
      L[3 * i + 1] = g;
    }
  }
  if (!rdOk) {
    t_fail = 1;
    return false;
  }
  // ---------------------------------------------------------------- phase 2 (0x82107D64)
  uint8_t fmtBytes[64];
  for (uint32_t k = 0; k < 64; k++) fmtBytes[k] = uint8_t(rd8(kFmtBytes + k));
  auto ent = [&](uint32_t k) { return rd32(table + 4u * k); };
  uint32_t p = 0;  // r27 as an index from r30
  while (p < r26) {
    while (p < r26 && !(ent(p) & 0x300000u)) p++;
    if (p >= r26) break;
    const uint32_t start = p;
    uint32_t q = p + 1;
    if (q >= r26) break;
    while (q < r26 && !(ent(q) & 0x300000u)) q++;
    if (q >= r26) break;
    const uint32_t endE = ent(q);
    const uint32_t dist = q - start;
    const uint32_t r6 = dist + 1;
    uint32_t* r3 = &L[3 * start];
    uint32_t r5 = r6;
    uint32_t r10 = dist;
    int32_t r31 = -1;
    p = q + 1;
    if ((endE & 0x300000u) == 0x300000u) {
      r5 = dist;
      r10 = dist - 1;
      r31 = int32_t(rlwinm(keyWord(L[3 * q]), 14, 25, 31));
    }
    if (r10 != 0) {  // selection sort (0x82107E50)
      uint32_t r4 = 1, outer = r10;
      uint32_t* a = r3;
      for (;;) {
        if (r4 < r5) {
          uint32_t* bb = a + 3;
          uint32_t inner = r5 - r4;
          do {
            const int32_t kA = int32_t(rlwinm(keyWord(a[0]), 14, 25, 31));
            const int32_t kB = int32_t(rlwinm(keyWord(bb[0]), 14, 25, 31));
            int32_t r9 = kB - kA;
            bool swap;
            if (r9 == 0) {
              r9 = fetchOffset(a[2]) - fetchOffset(bb[2]);
              if (r9 == 0) r9 = int32_t(fmtBytes[(a[1] >> 16) & 0x3Fu]) - int32_t(fmtBytes[(bb[1] >> 16) & 0x3Fu]);
              swap = r9 > 0;
            } else if (r31 == kA) {
              swap = true;
            } else if (r31 == kB) {
              swap = false;
            } else {
              swap = r9 > 0;
            }
            if (swap)
              for (int k = 0; k < 3; k++) {
                const uint32_t tmp = a[k];
                a[k] = bb[k];
                bb[k] = tmp;
              }
            bb += 3;
          } while (--inner);
        }
        a += 3;
        r4++;
        if (--outer == 0) break;
      }
    }
    // mini-fetch merge (0x82107F3C)
    uint32_t* endp = r3 + 3 * r6;
    uint32_t* base = r3;
    for (uint32_t* cur = r3 + 3; cur < endp; cur += 3) {
      const uint32_t b0 = base[0];
      if ((keyWord(b0) & 0x01FC0000u) != (keyWord(cur[0]) & 0x01FC0000u)) {
        base = cur;
        continue;
      }
      const uint32_t offBase = uint32_t(fetchOffset(base[2])), offCur = uint32_t(fetchOffset(cur[2]));
      if (offBase > offCur) {
        base = cur;
        continue;
      }
      uint32_t span = fmtBytes[(cur[1] >> 16) & 0x3Fu] - offBase + offCur;
      if (span > 8u) {
        base = cur;
        continue;
      }
      const uint32_t r9 = (b0 >> 27) & 7u;
      span -= 1u;
      if (!(r9 <= span)) span = r9;
      base[0] = rlwimi(b0, span, 27, 2, 4);
      cur[1] |= 0x40000000u;
    }
  }
  // ---------------------------------------------------------------- phase 3 (0x82107FF4)
  for (uint32_t i = 0; i < r26; i++) {
    const int64_t at = outBias + int64_t(12u) * (ent(i) & 0xFFFu);
    if (at < 0 || uint64_t(at) + 12u > outBytes) {
      t_fail = 3;
      return false;
    }
  }
  if (!rdOk) {
    t_fail = 1;
    return false;
  }
  for (uint32_t i = 0; i < r26; i++) {
    uint8_t* dst = out + outBias + 12u * (ent(i) & 0xFFFu);
    for (int k = 0; k < 3; k++) {
      const uint32_t v = _byteswap_ulong(L[3 * i + k]);
      std::memcpy(dst + 4 * k, &v, 4);
    }
  }
  return true;
}

}  // namespace sr::native::vspatch
