// NATIVE cloth light mask (iter-467 guestcode round 4): ClothUpdateElementLightMask
// sub_823055A8 with its culling callees MathVmxDotMinReduce sub_82319108,
// BoundsVolumeIntersectTest sub_8231A5F8, BoxOverlapTest sub_8231A408 and the camera getter
// sub_82775BF0 as host code. 1.74 ms of the 8.85 ms player-profile render tick
// (docs/PERFORMANCE.md 2.11).
//
// WHAT IT IS. A register-level transcription of the recompiled guest bodies (IDA listings +
// guest/ppc lowering): every guest register is a host local, every guest MEMORY access the
// original makes outside its own stack frame is made here too, at the same program point, with
// the same alias fold (so data the loop itself rewrites - bucket records, counters - is re-read
// exactly as the guest re-reads it). Every VMX op is the SAME simde expression the recompiler
// emits (vmaddfp = mul then add, vmsum3/4fp128 = simde's portable dp_ps, vrefp / vrsqrtefp =
// IEEE divides, vperm = simde_mm_perm_epi8_), compiled at the guest lib's ISA level (-mssse3,
// no SSE4.1/FMA), in the guest's VMX flush mode. The scalar FPU parts are exact without a mode
// switch: lfs->stfs copies are bit copies with the SNaN quieting the IEEE-mode double round
// trip performs, fcmpu on lfs values is an exact integer compare (unordered = false); the three
// fmuls of the prologue run in IEEE mode exactly as the guest runs them. Stack temporaries
// (stvx then lvx/lfs of the same slot) are the register values themselves; the dead frame is
// not written.
//
// GATES. SR_NATIVE_CLOTHMASK (default ON since iter-467; =0 runs the guest body): run natively.
// SR_NATIVE_CLOTHMASK_CHECK=1: per call, snapshot every region either implementation can
// write (the element-set object, the per-element mask bytes, the LOD bucket arrays the cursors
// point at, the callees' once-init statics), run the guest original, keep its result, restore
// the snapshot and the inputs, run the native version, compare return value + every byte, then
// put the guest's result back. Counts every call (activation witness) and logs mismatches.
// Any precondition the transcription does not cover (more than 4 LOD buckets) falls back to
// the guest body.
// C1: the CHECKs (SR_NATIVE_CLOTHMASK_CHECK, SR_NATIVE_CLOTHBOUNDS_CHECK) and the timing A/Bs (_AB) are development
// oracles, compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp): there those gates read as unset (no env read). The
// native paths, their fallbacks to the guest body and the [NATIVE_CLOTHMASK] counter line stay (its CHECK counters
// then read 0, as with the CHECK unset).

#include "native_vmx.hpp"
#include "ab_toggle.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <thread>
#include <vector>

extern "C" void __imp__sub_823055A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82305B88(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::cloth {
namespace {

// the gates (literal env names: scripts/audit_forcing.py reads these as gate predicates)
bool clothMaskOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHMASK"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_CLOTHMASK");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool clothMaskCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHMASK_CHECK"); return e && e[0] == '1'; }();
  return v;
}

bool clothMaskAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHMASK_AB"); return e && e[0] == '1'; }();
  return v;
}
#endif  // SR_ORACLE

std::atomic<uint64_t> g_calls{0}, g_native{0}, g_fallback{0}, g_checked{0}, g_bad{0}, g_elems{0}, g_unchk{0};
#if SR_ORACLE
sr::native::vmx::AbTimer g_ab;
thread_local bool t_parNow = true;  // the AB timer's single-thread native mode clears it
#else
constexpr bool t_parNow = true;  // SR_ORACLE=0: no A/B, nothing clears it
#endif

using namespace sr::native::vmx;
// guest constants (lfs sources)
constexpr uint32_t kC13B8 = 0x820013B8u, kC13C0 = 0x820013C0u, kC21360 = 0x82021360u;

// ---- sub_8231A408 BoxOverlapTest(r4 -> A, r5 -> B) -----------------------------------------
// The callees' once-init statics: with `touch` (the threaded compute pass) a missing static is
// computed exactly as the init would store it but NOT written; the touch bit makes the in-order
// apply pass perform the guest's init writes (kTouch*Init) at that element.
constexpr uint8_t kTouchDot = 1, kTouchBounds = 2, kTouchBox = 4;

uint32_t boxOverlap(uint8_t* b, V A, V B, uint8_t* touch = nullptr) {
  uint32_t fl = lwz(b, 0x829702E0u);
  const uint32_t f0 = lwz(b, kC13B8), f13 = lwz(b, kC13C0);
  const bool w = touch == nullptr;
  if (!w && (fl & 0xF) != 0xF) *touch |= kTouchBox;
  V v11, v0, v13, v12;
  if (!(fl & 1)) {
    fl |= 1;
    if (w) stw(b, 0x829702E0u, fl);
    v11 = image(fcopy(f13), fcopy(f0), fcopy(f0), fcopy(f0));
    if (w) stvx(b, 0x829702D0u, v11);
  } else {
    v11 = lvx(b, 0x829702D0u);
  }
  const V v10 = vsub(B, A);
  if (!(fl & 2)) {
    fl |= 2;
    if (w) stw(b, 0x829702E0u, fl);
    v0 = image(fcopy(f0), fcopy(f0), fcopy(f0), 0);
    if (w) stvx(b, 0x829702C0u, v0);
  } else {
    v0 = lvx(b, 0x829702C0u);
  }
  if (!(fl & 4)) {
    fl |= 4;
    if (w) stw(b, 0x829702E0u, fl);
    v13 = image(fcopy(f13), fcopy(f13), fcopy(f13), 0);
    if (w) stvx(b, 0x829702B0u, v13);
  } else {
    v13 = lvx(b, 0x829702B0u);
  }
  if (!(fl & 8)) {
    v12 = vsub(v13, v0);
    fl |= 8;
    if (w) stw(b, 0x829702E0u, fl);
    if (w) stvx(b, 0x829702A0u, v12);
  } else {
    v12 = lvx(b, 0x829702A0u);
  }
  v12 = vadd(v10, v12);
  v0 = vmin(A, v0);
  v13 = vmax(B, v13);
  v0 = vsub(v13, v0);
  v0 = vsub(v0, v12);
  // bge = !lt: an unordered compare (NaN) takes the branch, as the guest's `if (!cr6.lt)`
  if (!fLT(elem(v0, 0), elem(v11, 1))) return 0;
  if (!fLT(elem(v0, 1), elem(v11, 1))) return 0;
  return fLT(elem(A, 2), elem(v11, 0)) ? 1u : 0u;
}

// ---- sub_82319108 MathVmxDotMinReduce(r3 planes, r4 -> point, r5 -> radius) -------------
uint32_t dotMinReduce(uint8_t* b, uint32_t planes, V point, V radius, uint8_t* touch = nullptr) {
  uint32_t fl = lwz(b, 0x82970280u);
  V v10;
  if (!(fl & 1)) {
    const uint32_t a = lwz(b, kC21360), c = lwz(b, kC13B8);
    fl |= 1;
    if (!touch) stw(b, 0x82970280u, fl);
    v10 = image(fcopy(a), fcopy(c), 0, 0);
    if (!touch) stvx(b, 0x82970270u, v10);
    else *touch |= kTouchDot;
  } else {
    v10 = lvx(b, 0x82970270u);
  }
  V v0 = vperm(v10, v10, spltw0(lvsl(0)));
  uint32_t p = planes;
  for (int i = 0; i < 5; ++i, p += 16) {
    V v13 = lvx(b, p);
    v13 = dp4(v13, point);
    v13 = vadd(v13, radius);
    v0 = vmin(v0, v13);
  }
  return fLT(elem(v0, 0), elem(v10, 1)) ? 0u : 1u;
}

// ---- sub_8231A5F8 BoundsVolumeIntersectTest ------------------------------------------------
// r3 lights, r4 -> point, r5 -> radius, r6 light index, r7 -> v86, r8 -> v85, r9 matrix.
// phaseB = (r7 != 0): ClothUpdateBoundsAndLights passes r7 = 0 and gets 1 after phase A.
uint32_t boundsVolume(uint8_t* b, uint32_t r31, V v8, V radius, uint32_t r25, V v86, V v85, uint32_t r30,
                      uint8_t* touch = nullptr, bool phaseB = true) {
  V v0 = lvx(b, ((r25 + 36u) << 4) + r31);
  V v13 = spltw3(v0);
  v0 = vsub(v8, v0);
  V v12 = lvx(b, r31 + 656);
  v13 = vadd(radius, v13);
  V v11 = dp3(v0, v12);
  v13 = vmul(v13, v13);
  v12 = vmul(v12, v11);
  v0 = vsub(v0, v12);
  v0 = dp3(v0, v0);
  if (gtMask(v13, v0) == 0) return 0;
  uint32_t fl = lwz(b, 0x82970300u);
  V v6;
  if (!(fl & 1)) {
    const uint32_t f0a = lwz(b, kC13C0), f0b = lwz(b, kC13B8);
    fl |= 1;
    if (!touch) stw(b, 0x82970300u, fl);
    v6 = image(fcopy(f0a), fcopy(f0b), fcopy(f0b), fcopy(f0b));
    if (!touch) stvx(b, 0x829702F0u, v6);
    else *touch |= kTouchBounds;
  } else {
    v6 = lvx(b, 0x829702F0u);
  }
  const V v6init = v6;  // == the static at 0x829702F0 from here on
  // phase A: the light's box from the sphere, through the light's three transform rows
  v0 = radius;
  v13 = lvx(b, r31 + 672);
  v12 = lvx(b, r31 + 688);
  v13 = vadd(v12, v13);
  v12 = lvx(b, r31 + 656);
  v11 = lvx(b, r31 + 304);
  V v10 = lvx(b, r31 + 288);
  V v9 = lvx(b, r31 + 320);
  V v7 = lvx(b, 0x829112A0u);
  V v5 = vadd(v13, v12);
  v13 = vadd(v13, v12);
  v12 = vmul(v5, v0);
  v13 = vmul(v13, v0);
  v0 = vsub(v8, v12);
  v13 = vadd(v8, v13);
  v0 = rlimi_1_1(v0, v6);
  v12 = dp4(v0, v11);
  v8 = dp4(v0, v10);
  v13 = rlimi_1_1(v13, v6);
  v6 = dp4(v0, v9);
  v0 = vperm(v8, v12, v7);
  v12 = dp4(v13, v11);
  v11 = dp4(v13, v10);
  v10 = dp4(v13, v9);
  v0 = rlimi_2_0(v0, v6);
  v13 = vperm(v11, v12, v7);
  v13 = rlimi_2_0(v13, v10);
  if (r25 != 0) {
    const V lv10 = lvsl(12);
    const V v6s = spltw0(lv10);
    uint32_t r11 = r31 + 336;
    for (uint32_t r10 = r25; r10 != 0; --r10) {
      const V v7s = spltw0(lv10);
      V row = lvx(b, r11);
      const V v11r = row;
      row = vperm(row, row, v7s);
      v0 = vmul(v0, row);
      v0 = vadd(v0, v11r);
      row = lvx(b, r11);
      r11 += 16;
      row = vperm(row, row, v6s);
      v13 = vmul(v13, row);
      v13 = vadd(v13, v11r);
    }
  }
  if ((boxOverlap(b, v0, v13, touch) & 0xFF) == 0) return 0;
  if (!phaseB) return 1;  // cmplwi r24,0; bne 0x8231a870; li r3,1
  // phase B (r7 = the caller's stack slot)
  V a0 = v86;
  V b13 = touch ? v6init : lvx(b, 0x829702F0u);
  V v11b = lvx(b, r30);
  const V v12l = lvsl(0);
  a0 = rlimi_1_1(a0, b13);
  V c6 = spltw0(v12l);
  const V l8 = lvsl(8);
  const V c5 = spltw0(l8);
  const V v8l = lvsl(12);
  V v10b = lvx(b, r30 + 16);
  const V c4 = spltw0(v8l);
  V t13 = vperm(a0, a0, c6);
  V c7 = spltw0(lvsl(4));
  t13 = vmul(v11b, t13);
  V w6 = vperm(a0, a0, c4);
  V v3 = lvx(b, r30 + 48);
  V v4 = lvx(b, 0x829112A0u);
  V w12 = vperm(a0, a0, c7);
  V v9b = lvx(b, r30 + 32);
  V w7 = vperm(a0, a0, c5);
  V v5b = lvx(b, r31 + 320);
  t13 = vmadd(v10b, w12, t13);
  t13 = vmadd(v9b, w7, t13);
  V v7b = lvx(b, r31 + 304);
  t13 = vmadd(v3, w6, t13);
  V v6b = lvx(b, r31 + 288);
  t13 = rlimi_1_0(t13, a0);
  V p0 = t13;
  V p3 = dp4(p0, v7b);
  V p2 = dp4(p0, v6b);
  V s13 = v85;
  V s12 = v86;
  const V v31 = spltw0(s13);
  const V v30 = spltw0(s12);
  V p1 = dp4(p0, v5b);
  const V v29 = spltw1(s13);
  const V v28 = spltw1(s12);
  s13 = spltw2(s13);
  s12 = spltw2(s12);
  s13 = vsub(s13, s12);
  p0 = vperm(p2, p3, v4);
  p3 = vsub(v31, v30);
  p2 = vsub(v29, v28);
  p0 = rlimi_2_0(p0, p1);
  s13 = vmul(v9b, s13);
  V T[3];
  T[0] = vmul(v11b, p3);
  T[1] = vmul(v10b, p2);
  T[2] = s13;
  V U[3];
  for (int i = 0; i < 3; ++i) {
    V t = T[i];
    V q12 = dp4(t, v7b);
    V q11 = dp4(t, v6b);
    V q10 = dp4(t, v5b);
    t = vperm(q11, q12, v4);
    U[i] = rlimi_2_0(t, q10);
  }
  if (r25 != 0) {
    uint32_t r9 = r31 + 336;
    for (uint32_t n = r25; n != 0; --n) {
      const V q7 = spltw0(v8l);
      V row = lvx(b, r9);
      const V raw = row;
      const V q6 = spltw0(v8l);
      row = vperm(row, row, q7);
      p0 = vmul(p0, row);
      p0 = vadd(p0, raw);
      row = lvx(b, r9);
      row = vperm(row, row, q6);
      for (int j = 0; j < 3; ++j) U[j] = vmul(U[j], row);
      r9 += 16;
    }
  }
  V mx = p0;
  for (int j = 0; j < 3; ++j) {
    const V u = U[j];
    const V s11 = vadd(p0, u);
    const V s12b = vadd(mx, u);
    p0 = vmin(p0, s11);
    mx = vmax(mx, s12b);
  }
  return (boxOverlap(b, p0, mx, touch) & 0xFF) != 0 ? 1u : 0u;
}

// the init writes a callee performs on its first call (the apply pass of the threaded loop)
void initWritesDot(uint8_t* b) {
  const uint32_t fl = lwz(b, 0x82970280u);
  if (fl & 1) return;
  const uint32_t a = lwz(b, kC21360), c = lwz(b, kC13B8);
  stw(b, 0x82970280u, fl | 1);
  stvx(b, 0x82970270u, image(fcopy(a), fcopy(c), 0, 0));
}
void initWritesBounds(uint8_t* b) {
  const uint32_t fl = lwz(b, 0x82970300u);
  if (fl & 1) return;
  const uint32_t f0a = lwz(b, kC13C0), f0b = lwz(b, kC13B8);
  stw(b, 0x82970300u, fl | 1);
  stvx(b, 0x829702F0u, image(fcopy(f0a), fcopy(f0b), fcopy(f0b), fcopy(f0b)));
}
void initWritesBox(uint8_t* b) {
  const V z = simde_mm_setzero_ps();
  (void)boxOverlap(b, z, z);  // with writes on: the four inits, then reads of the statics only
}

// The three products of the prologue, in the guest's IEEE (non-flush) mode. noinline keeps
// them on the right side of the mode switches.
struct Prologue { uint32_t v76, v77, v81; };
__declspec(noinline) Prologue prologueProducts(uint8_t* b) {
  float f;
  auto ldf = [&](uint32_t va) { uint32_t u = lwz(b, va); std::memcpy(&f, &u, 4); return double(f); };
  const double cCCC = ldf(0x82913CCCu), cCE0 = ldf(0x82913CE0u);
  const double f12 = double(float(cCCC * cCE0));
  const double cCC0 = ldf(0x82913CC0u), c3E0 = ldf(0x820203E0u);
  const double p76 = double(float(cCC0 * c3E0));
  const double cCD0 = ldf(0x82913CD0u), cCE8 = ldf(0x82913CE8u);
  const double f13 = double(float(cCD0 * cCE8));
  const double c3E4 = ldf(0x820203E4u);
  const double p77 = double(float(f12 * c3E4));
  const double c3E8 = ldf(0x820203E8u);
  const double p81 = double(float(f13 * c3E8));
  Prologue r;
  float t;
  t = float(p76); std::memcpy(&r.v76, &t, 4);
  t = float(p77); std::memcpy(&r.v77, &t, 4);
  t = float(p81); std::memcpy(&r.v81, &t, 4);
  return r;
}

// ---- THREADED element pass (SR_NATIVE_CLOTHMASK_THREADS=N workers) -------------------------
// Per element the transform, the visibility test, the LOD distance and the per-light mask only
// READ memory (once the callees' once-init statics exist); the bucket appends, counters and
// mask bytes are the only writes. So: compute every element on N+1 threads, then apply the
// writes on the calling thread in element order - the guest's own order. Taken only when no
// write range can overlap anything the compute pass reads (else the single-thread loop runs).
// MEASURED (iter-467, in-process timing roll cm6abt / cm7abt96): per call guest 2.82 us, native
// 1.44 us, threaded 1.76 us (MINPAR 16) / 1.60 us (MINPAR 96) - the calls are small (~26
// elements) and the spinning workers share cores with the render thread, so the threaded pass
// is OFF by default (THREADS=0). Exact under CHECK (cm7chk3: 2.22M parallel calls, 0 bad).
int envInt(const char* n, int def) {
  const char* e = std::getenv(n);
  return e ? std::atoi(e) : def;
}
const int g_threads = envInt("SR_NATIVE_CLOTHMASK_THREADS", 0);
const uint32_t g_minPar = uint32_t(envInt("SR_NATIVE_CLOTHMASK_MINPAR", 16));
std::atomic<uint64_t> g_parCalls{0}, g_parElems{0};

struct LoopIn {
  uint8_t* b;
  uint32_t a1, r20, r24, r18, r22, n53;
  V v126, v75, v86, v85, v125, v122, v123, v124;
  uint32_t a1_37, planes;
};
struct ElemOut {
  V d;
  uint32_t r3, r25bits;
  uint8_t vis, lod, mask, dset, touch;
};

// the per-element half of the loop body of elementLoop, without its writes
void computeElem(const LoopIn& c, uint32_t r26, ElemOut& o) {
  uint8_t* b = c.b;
  const uint32_t r31 = (r26 << 6) + c.a1_37;
  const V x0 = spltw0(c.v126), x1 = spltw1(c.v126), x2 = spltw2(c.v126), x3 = spltw3(c.v126);
  V v0 = vmul(lvx(b, r31), x0);
  v0 = vmadd(lvx(b, r31 + 16), x1, v0);
  v0 = vmadd(lvx(b, r31 + 32), x2, v0);
  v0 = vmadd(lvx(b, r31 + 48), x3, v0);
  const V v71 = v0;
  o.touch = 0;
  uint32_t r3 = dotMinReduce(b, c.planes, v71, c.v75, &o.touch);
  o.vis = (r3 & 0xFF) != 0;
  o.dset = 0;
  o.lod = 0;
  if (o.vis) {
    r3 = 0;
    V d = vsub(v71, c.v125);
    if (c.n53 > 1) {
      d = dp3(d, d);
      const V one = simde_mm_castsi128_ps(simde_mm_set1_epi32(1));
      const V zero = simde_mm_castsi128_ps(simde_mm_set1_epi32(0));
      V v9 = vcfsx0(one), v8 = vcfsx1(one);
      const V v7 = vcfsx0(one), v6 = vcfsx1(one);
      V v12 = vrsqrte(d);
      o.d = d;
      o.dset = 1;
      V v11 = simde_mm_cmpeq_ps(zero, d);
      V v5 = vmul(v12, v12);
      v8 = vmul(v12, v8);
      v5 = vmul(d, v5);
      v9 = vsub(v9, v5);
      v9 = vmul(v8, v9);
      v12 = vadd(v12, v9);
      v9 = vmul(v12, v12);
      v8 = vmul(v12, v6);
      v9 = vmul(d, v9);
      v9 = vsub(v7, v9);
      v9 = vmul(v8, v9);
      v8 = c.v75;
      v12 = vadd(v12, v9);
      v9 = vmul(d, v12);
      v12 = vcfsx0(one);
      V v13 = vcfsx0(one);
      v11 = vsel(v9, zero, v11);
      const V v10 = vrefp(v11);
      v12 = vnmsub(v10, v11, v12);
      v12 = vmadd(v10, v12, v10);
      v13 = vnmsub(v12, v11, v13);
      v13 = vmadd(v12, v13, v12);
      v13 = vmul(v13, v8);
      bool two = false;
      if (c.n53 > 2) {
        if (gtMask(d, c.v122) != 0 || gtMask(c.v124, v13) == 0xF) two = true;
      }
      if (two) r3 = 2;
      else if (gtMask(d, c.v123) != 0) r3 = 1;
    }
    o.lod = uint8_t(r3);
  }
  uint32_t r27 = 0, bits = 0;
  for (int32_t r31l = int32_t(c.r18 - 1); r31l >= 0; --r31l) {
    const uint32_t sh = uint32_t(r31l) & 0xFF;
    const uint32_t r28 = (sh & 0x20) ? 0u : (1u << (sh & 0x1F));
    if ((r28 & c.r22) == 0) continue;
    r3 = boundsVolume(b, c.r24, v71, c.v75, uint32_t(r31l), c.v86, c.v85, r31, &o.touch);
    if ((r3 & 0xFF) == 0) break;
    r27 = ((r27 & 0xFF) | r28) & 0xFF;
    bits |= (sh & 0x20) ? 0u : (2u << (sh & 0x1F));
  }
  o.mask = uint8_t(r27);
  o.r25bits = bits;
  o.r3 = r3;
}

struct Range { uint32_t lo, hi; };  // [lo, hi)
bool overlaps(const Range& a, const Range& b) { return a.lo < b.hi && b.lo < a.hi; }

std::atomic<uint64_t> g_inel[12];  // why a call took the single-thread loop (report())

bool inel(int why) {
  g_inel[why].fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool parallelEligible(LoopIn& c, const uint32_t cur[4]) {
  uint8_t* b = c.b;
  if (c.n53 == 0 || c.r18 > 64 || c.r20 > 0x10000) return inel(0);
  c.a1_37 = lwz(b, c.a1 + 148);
  c.planes = lwz(b, 0x82970154u);
  const uint32_t a1_51 = lwz(b, c.a1 + 204);
  const bool rec32 = lwz(b, c.a1 + 232) == 2;
  const uint32_t rec = rec32 ? 32u : 64u;
  Range W[5];
  int nw = 0;
  for (uint32_t i = 0; i < c.n53; ++i) W[nw++] = {cur[i], cur[i] + rec * c.r20};
  W[nw++] = {a1_51, a1_51 + c.r20};
  // a1[57] (16-byte records) is read only by the 32-byte record form
  const uint32_t a1_57 = rec32 ? lwz(b, c.a1 + 228) : 0x82970150u;
  const Range R[] = {
      {c.a1, c.a1 + 256},
      {c.a1_37, c.a1_37 + 64 * c.r20},
      {c.planes, c.planes + 80},
      {c.r24, c.r24 + 1024 + 32 * c.r18},
      {0x82970150u, 0x82970310u},
      {0x829112A0u, 0x829112B0u},
      {a1_57, a1_57 + (rec32 ? 16 * c.r20 : 0x1C0u)},
  };
  int k = 0;
  for (const Range& r : R) {
    if (r.hi < r.lo || r.hi > 0xA0000000u) return inel(1 + k);  // wrap / alias windows: not modelled
    ++k;
  }
  for (int i = 0; i < nw; ++i) {
    if (W[i].hi < W[i].lo || W[i].hi > 0xA0000000u) return inel(8);
    for (const Range& r : R)
      if (overlaps(W[i], r)) return inel(i == nw - 1 ? 9 : 10);
  }
  return true;
}

// a small persistent pool. The calls come in bursts (hundreds of element sets per frame, a few
// microseconds apart), so a worker spins on the job generation for a while before it sleeps
// (WaitOnAddress underneath); the poster only issues the wake when someone sleeps. The caller
// works too and waits for every worker to check in.
class Pool {
 public:
  explicit Pool(int n) : n_(n) {
    for (int i = 0; i < n; ++i) th_.emplace_back([this] { loop(); });
  }
  void run(const LoopIn* in, ElemOut* out, uint32_t count) {
    in_ = in;
    out_ = out;
    count_ = count;
    csr_ = _mm_getcsr();
    next_.store(0, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_seq_cst) > 0) gen_.notify_all();
    work();
    while (done_.load(std::memory_order_acquire) != n_) _mm_pause();
  }

 private:
  static constexpr uint32_t kChunk = 4;
  static constexpr uint64_t kSpinTicks = 200000;  // ~50-70 us of TSC
  void work() {
    for (;;) {
      const uint32_t s = next_.fetch_add(kChunk, std::memory_order_relaxed);
      if (s >= count_) break;
      const uint32_t e = s + kChunk < count_ ? s + kChunk : count_;
      for (uint32_t k = s; k < e; ++k) computeElem(*in_, k, out_[k]);
    }
  }
  void loop() {
    uint32_t seen = 0;  // the generation at construction: a job posted before this thread ran is still seen
    for (;;) {
      uint32_t g = gen_.load(std::memory_order_acquire);
      if (g == seen) {
        const uint64_t t0 = __rdtsc();
        while ((g = gen_.load(std::memory_order_acquire)) == seen && __rdtsc() - t0 < kSpinTicks) _mm_pause();
      }
      if (g == seen) {
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        while ((g = gen_.load(std::memory_order_seq_cst)) == seen) gen_.wait(seen, std::memory_order_seq_cst);
        sleepers_.fetch_sub(1, std::memory_order_relaxed);
      }
      seen = g;
      _mm_setcsr(csr_);
      work();
      done_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  int n_;
  std::vector<std::thread> th_;
  std::atomic<uint32_t> gen_{0}, next_{0};
  std::atomic<int> done_{0}, sleepers_{0};
  const LoopIn* in_ = nullptr;
  ElemOut* out_ = nullptr;
  uint32_t count_ = 0;
  unsigned csr_ = 0;
};

Pool& pool() {
  static Pool* p = new Pool(g_threads);  // never destroyed: workers live as long as the process
  return *p;
}

void parallelLoop(const LoopIn& c, uint32_t cur[4], V& v127, uint32_t& r25, uint32_t& r3) {
  uint8_t* b = c.b;
  thread_local std::vector<ElemOut> outs;
  if (outs.size() < c.r20) outs.resize(c.r20);
  pool().run(&c, outs.data(), c.r20);
  for (uint32_t r26 = 0; r26 < c.r20; ++r26) {
    const ElemOut& o = outs[r26];
    if (o.touch & kTouchDot) initWritesDot(b);
    if (o.vis) {
      r25 |= 1;
      if (o.dset) v127 = o.d;
      const uint32_t idx = o.lod & 3;
      const uint32_t r31 = (r26 << 6) + lwz(b, c.a1 + 148);
      if (lwz(b, c.a1 + 232) == 2) {
        const uint32_t row3 = r31 + 48;
        const uint64_t q0 = ld(b, row3);
        const uint32_t rec = cur[idx];
        sd(b, rec, q0);
        cur[idx] = rec + 32;
        sd(b, rec + 8, ld(b, row3 + 8));
        stw(b, rec + 12, fcopy(elem(v127, 0)));
        const uint32_t src = (r26 << 4) + lwz(b, c.a1 + 228);
        sd(b, rec + 16, ld(b, src));
        sd(b, rec + 24, ld(b, src + 8));
      } else {
        const uint32_t rec = cur[idx];
        const uint64_t q0 = ld(b, r31);
        sd(b, rec, q0);
        const uint64_t q1 = ld(b, r31 + 8);
        sd(b, rec + 8, q1);
        sd(b, rec + 16, ld(b, r31 + 16));
        sd(b, rec + 24, ld(b, r31 + 24));
        sd(b, rec + 32, ld(b, r31 + 32));
        cur[idx] = rec + 64;
        sd(b, rec + 40, ld(b, r31 + 40));
        sd(b, rec + 48, ld(b, r31 + 48));
        sd(b, rec + 56, ld(b, r31 + 56));
      }
      const uint32_t ca = c.a1 + ((uint32_t(o.lod) + 54) << 2);
      stw(b, ca, lwz(b, ca) + 1);
    }
    if (o.touch & kTouchBounds) initWritesBounds(b);
    if (o.touch & kTouchBox) initWritesBox(b);
    r25 |= o.r25bits;
    stb(b, lwz(b, c.a1 + 204) + r26, o.mask);
    r3 = o.r3;
  }
  g_elems.fetch_add(c.r20, std::memory_order_relaxed);
  g_parCalls.fetch_add(1, std::memory_order_relaxed);
  g_parElems.fetch_add(c.r20, std::memory_order_relaxed);
}

// The element loop of sub_823055A8 (VMX flush mode). Returns false (nothing written) when a
// precondition is outside the transcription.
__declspec(noinline) bool elementLoop(uint8_t* b, uint32_t a1, uint32_t v3, const Prologue& pr, uint32_t& r3out) {
  const uint32_t n53 = lwz(b, a1 + 212);
  if (n53 > 4) return false;
  // the stack cursor array v70[] starts as {v3[10], v3[11], v3[12], c(0x820013C0)} (stfs copies)
  uint32_t cur[4] = {fcopy(lwz(b, v3 + 40)), fcopy(lwz(b, v3 + 44)), fcopy(lwz(b, v3 + 48)),
                     fcopy(lwz(b, kC13C0))};
  const V v75 = spltw0(image(fcopy(lwz(b, v3 + 52)), 0, 0, 0));
  const V v86 = image(fcopy(lwz(b, v3 + 16)), fcopy(lwz(b, v3 + 20)), fcopy(lwz(b, v3 + 24)), 0);
  const V v85 = image(fcopy(lwz(b, v3 + 28)), fcopy(lwz(b, v3 + 32)), fcopy(lwz(b, v3 + 36)), 0);
  const V v126 = image(cur[0], cur[1], cur[2], cur[3]);
  if (n53 != 0) {
    uint32_t r11 = a1 + 236;
    for (uint32_t i = 0;;) {
      const uint32_t r8 = lwz(b, r11);
      ++i;
      stw(b, r11 - 20, 0);
      r11 += 4;
      const uint32_t r7 = lwz(b, a1 + 212);
      cur[i - 1] = r8;
      if (!(i < r7)) break;
      if (i >= 4) return false;  // unreachable while a1[53] <= 4 (it was re-read above)
    }
  }
  // the camera getters: sub_82775BD8's result is unused; sub_82775BF0 -> +0x70
  const uint32_t g = lwz(b, 0x829A9384u);
  {
    const uint32_t t = lwz(b, lwz(b, g + 96));
    (void)lwz(b, t + 0);  // sub_82775BD8 (index 0): a pure load, kept for the access pattern
  }
  const uint32_t bf0 = lwz(b, lwz(b, lwz(b, g + 96))) + 112;
  uint32_t r3 = bf0;
  const uint32_t r20 = lwz(b, a1 + 152);
  const uint32_t r24 = lwz(b, 0x82970190u);
  const uint32_t r18 = lwz(b, r24 + 720);
  const V v123 = [&] { V t = spltw0(image(pr.v76, 0, 0, 0)); return vmul(t, t); }();
  const V v125 = lvx(b, bf0 + 48);
  const V v122 = [&] { V t = spltw0(image(pr.v77, 0, 0, 0)); return vmul(t, t); }();
  const V v124 = spltw0(image(pr.v81, 0, 0, 0));
  const uint8_t r6 = lbz(b, 0x82970260u);
  const uint32_t w1A8 = lwz(b, 0x829701A8u);
  if (r6 == 0 && w1A8 == 0) stb(b, 0x82970260u, 1);
  uint32_t r9 = 0x8296FEA0u;
  {
    const uint32_t r10 = lwz(b, a1 + 64);
    if (w1A8 != 0 && r10 <= 31) {
      const uint32_t off = r10 * 20;
      if (int32_t(lwz(b, off + 0x8296FEB0u)) != -1) r9 = off + 0x8296FEA0u;
    }
  }
  const uint32_t r22 = lwz(b, r9 + 16);
  uint32_t r25 = 0;
  const uint32_t cB8 = fcopy(lwz(b, kC13B8));
  V v127 = spltw0(image(cB8, 0, 0, 0));
  if (r20 != 0 && g_threads > 0 && t_parNow && r20 < g_minPar) (void)inel(11);
  if (r20 != 0 && g_threads > 0 && t_parNow && r20 >= g_minPar) {
    LoopIn in{b, a1, r20, r24, r18, r22, n53, v126, v75, v86, v85, v125, v122, v123, v124, 0, 0};
    if (parallelEligible(in, cur)) {
      parallelLoop(in, cur, v127, r25, r3);
      stw(b, a1 + 48, r25);
      r3out = r3;
      return true;
    }
  }
  if (r20 != 0) {
    uint64_t elems = 0;
    for (uint32_t r26 = 0; r26 < r20; ++r26) {
      ++elems;
      const uint32_t r31 = (r26 << 6) + lwz(b, a1 + 148);
      const V x0 = spltw0(v126), x1 = spltw1(v126), x2 = spltw2(v126), x3 = spltw3(v126);
      const uint32_t planes = lwz(b, 0x82970154u);
      V v0 = vmul(lvx(b, r31), x0);
      v0 = vmadd(lvx(b, r31 + 16), x1, v0);
      v0 = vmadd(lvx(b, r31 + 32), x2, v0);
      v0 = vmadd(lvx(b, r31 + 48), x3, v0);
      const V v71 = v0;
      r3 = dotMinReduce(b, planes, v71, v75);
      if ((r3 & 0xFF) != 0) {
        const uint32_t r11n = lwz(b, a1 + 212);
        r25 |= 1;
        r3 = 0;
        V d = vsub(v71, v125);
        if (r11n > 1) {
          d = dp3(d, d);
          const V one = simde_mm_castsi128_ps(simde_mm_set1_epi32(1));
          const V zero = simde_mm_castsi128_ps(simde_mm_set1_epi32(0));
          V v9 = vcfsx0(one), v8 = vcfsx1(one);
          const V v7 = vcfsx0(one), v6 = vcfsx1(one);
          V v12 = vrsqrte(d);
          v127 = d;
          V v11 = simde_mm_cmpeq_ps(zero, d);
          V v5 = vmul(v12, v12);
          v8 = vmul(v12, v8);
          v5 = vmul(d, v5);
          v9 = vsub(v9, v5);
          v9 = vmul(v8, v9);
          v12 = vadd(v12, v9);
          v9 = vmul(v12, v12);
          v8 = vmul(v12, v6);
          v9 = vmul(d, v9);
          v9 = vsub(v7, v9);
          v9 = vmul(v8, v9);
          v8 = v75;
          v12 = vadd(v12, v9);
          v9 = vmul(d, v12);
          v12 = vcfsx0(one);
          V v13 = vcfsx0(one);
          v11 = vsel(v9, zero, v11);
          const V v10 = vrefp(v11);
          v12 = vnmsub(v10, v11, v12);
          v12 = vmadd(v10, v12, v10);
          v13 = vnmsub(v12, v11, v13);
          v13 = vmadd(v12, v13, v12);
          v13 = vmul(v13, v8);
          bool two = false;
          if (r11n > 2) {
            if (gtMask(d, v122) != 0 || gtMask(v124, v13) == 0xF) two = true;
          }
          if (two) r3 = 2;
          else if (gtMask(v127, v123) != 0) r3 = 1;
        }
        // append to bucket r3 (r10 == 1 always here)
        const uint32_t idx = r3 & 3;
        if (lwz(b, a1 + 232) == 2) {
          const uint32_t row3 = r31 + 48;
          const uint64_t q0 = ld(b, row3);
          const uint32_t rec = cur[idx];
          sd(b, rec, q0);
          cur[idx] = rec + 32;
          sd(b, rec + 8, ld(b, row3 + 8));
          stw(b, rec + 12, fcopy(elem(v127, 0)));
          const uint32_t src = (r26 << 4) + lwz(b, a1 + 228);
          sd(b, rec + 16, ld(b, src));
          sd(b, rec + 24, ld(b, src + 8));
        } else {
          const uint32_t rec = cur[idx];
          const uint64_t q0 = ld(b, r31);
          sd(b, rec, q0);
          const uint64_t q1 = ld(b, r31 + 8);
          sd(b, rec + 8, q1);
          sd(b, rec + 16, ld(b, r31 + 16));
          sd(b, rec + 24, ld(b, r31 + 24));
          sd(b, rec + 32, ld(b, r31 + 32));
          cur[idx] = rec + 64;
          sd(b, rec + 40, ld(b, r31 + 40));
          sd(b, rec + 48, ld(b, r31 + 48));
          sd(b, rec + 56, ld(b, r31 + 56));
        }
        const uint32_t ca = a1 + ((r3 + 54) << 2);
        stw(b, ca, lwz(b, ca) + 1);
      }
      // per-light mask
      uint32_t r27 = 0;
      for (int32_t r31l = int32_t(r18 - 1); r31l >= 0; --r31l) {
        const uint32_t sh = uint32_t(r31l) & 0xFF;
        const uint32_t r28 = (sh & 0x20) ? 0u : (1u << (sh & 0x1F));
        if ((r28 & r22) == 0) continue;
        const uint32_t mat = (r26 << 6) + lwz(b, a1 + 148);
        r3 = boundsVolume(b, r24, v71, v75, uint32_t(r31l), v86, v85, mat);
        if ((r3 & 0xFF) == 0) break;
        r27 = ((r27 & 0xFF) | r28) & 0xFF;
        r25 |= (sh & 0x20) ? 0u : (2u << (sh & 0x1F));
      }
      stb(b, lwz(b, a1 + 204) + r26, uint8_t(r27));
    }
    g_elems.fetch_add(elems, std::memory_order_relaxed);
  }
  stw(b, a1 + 48, r25);
  r3out = r3;
  return true;
}

// One native call. Returns false (nothing written, nothing changed) to fall back.
bool runNative(PPCContext& ctx, uint8_t* b) {
  const uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32;
  if (lwz(b, a1 + 212) > 4) return false;
  const uint32_t v3 = lwz(b, a2 + 68);
  ctx.fpscr.disableFlushMode();
  const Prologue pr = prologueProducts(b);
  ctx.fpscr.enableFlushMode();
  uint32_t r3 = 0;
  if (!elementLoop(b, a1, v3, pr, r3)) return false;
  ctx.r3.u64 = r3;
  return true;
}

void report() {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_CLOTHMASK] calls=%llu native=%llu fallback=%llu elements=%llu checked=%llu checkBad=%llu "
              "threads=%d parCalls=%llu parElems=%llu",
              (unsigned long long)g_calls.load(std::memory_order_relaxed),
              (unsigned long long)g_native.load(std::memory_order_relaxed),
              (unsigned long long)g_fallback.load(std::memory_order_relaxed),
              (unsigned long long)g_elems.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_bad.load(std::memory_order_relaxed), g_threads,
              (unsigned long long)g_parCalls.load(std::memory_order_relaxed),
              (unsigned long long)g_parElems.load(std::memory_order_relaxed));
#if SR_ORACLE
  if (clothMaskAbOn()) {
    double us[3];
    g_ab.means(us);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_CLOTHMASK] AB us/call guest=%.3f native=%.3f threaded=%.3f (calls %llu / %llu / %llu)", us[0],
                us[1], us[2], (unsigned long long)g_ab.calls[0].load(), (unsigned long long)g_ab.calls[1].load(),
                (unsigned long long)g_ab.calls[2].load());
  }
  if (clothMaskCheckOn())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CLOTHMASK] unchecked=%llu",
                (unsigned long long)g_unchk.load(std::memory_order_relaxed));
#endif  // SR_ORACLE
  if (g_threads > 0) {
    char buf[256];
    int n = 0;
    for (int i = 0; i < 12; ++i)
      n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " %llu",
                         (unsigned long long)g_inel[i].load(std::memory_order_relaxed));
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CLOTHMASK] single-thread reasons:%s", buf);
  }
}


// ---- sub_82305B88 ClothUpdateBoundsAndLights (SR_NATIVE_CLOTHBOUNDS) -----------------------
// Per cloth object (r3): refresh the snapshot (vtable +28 with r29[2]), its world matrix into
// r29+128 (vtable +48 = PoseSnapshot_GetWorldMatrix), the cloth's bounding sphere centre = that
// matrix applied to (v3[10], v3[11], v3[12], 1) with radius v3[13] (v3 = *(r28+68)), the frustum
// test (flag 0x80 of r29[4] when culled), the per-light mask (2 << light for every light whose
// bounds the sphere intersects, BoundsVolumeIntersectTest with r7 = 0) into r29[12], then
// vtable +88 with the centre, and flag 0x200. The guest calls stay guest calls (same targets,
// same r3 / r4, same frame: stwu r1,-256, the r29 store at r1+276, the centre at r1+160 for
// +88); the arithmetic and the culling callees run natively. The first call for an object
// (r29[55] == 0: it allocates the snapshot) runs the guest body.
bool clothBoundsOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHBOUNDS"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_CLOTHBOUNDS");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool clothBoundsCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHBOUNDS_CHECK"); return e && e[0] == '1'; }();
  return v;
}
bool clothBoundsAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CLOTHBOUNDS_AB"); return e && e[0] == '1'; }();
  return v;
}
#endif  // SR_ORACLE
std::atomic<uint64_t> g_bCalls{0}, g_bNative{0}, g_bFall{0}, g_bChecked{0}, g_bBad{0};
#if SR_ORACLE
sr::native::vmx::AbTimer g_bAb;
#endif

struct BoundsResult { V point; uint32_t mask; bool vis; uint8_t touchDummy; };

// The arithmetic half, from the object's current state (after the two refresh calls).
BoundsResult boundsCompute(uint8_t* b, uint32_t r29, uint32_t r28, bool writeStatics) {
  BoundsResult o{};
  const uint32_t fl = lwz(b, 0x82970370u);
  const uint32_t v15 = lwz(b, r28 + 68);
  V v10;
  if (!(fl & 1)) {
    v10 = spltw0(image(fcopy(lwz(b, kC13C0)), 0, 0, 0));
    if (writeStatics) {
      stw(b, 0x82970370u, fl | 1);
      stvx(b, 0x82970360u, v10);
    }
  } else {
    v10 = lvx(b, 0x82970360u);
  }
  const uint32_t r30 = r29 + 128;
  const V radius = spltw0(image(fcopy(lwz(b, v15 + 52)), 0, 0, 0));
  const V v8 = lvx(b, r30), v9 = lvx(b, r30 + 16), v3 = lvx(b, r30 + 32);
  V v0 = image(fcopy(lwz(b, v15 + 40)), fcopy(lwz(b, v15 + 44)), fcopy(lwz(b, v15 + 48)), fcopy(lwz(b, v15 + 52)));
  v0 = rlimi_1_0(v0, v10);
  const V xs = vperm(v0, v0, spltw0(lvsl(0))), ys = vperm(v0, v0, spltw0(lvsl(4)));
  const V zs = vperm(v0, v0, spltw0(lvsl(8))), ws = vperm(v0, v0, spltw0(lvsl(12)));
  V v13 = vmul(v8, xs);
  v13 = vmadd(v9, ys, v13);
  v13 = vmadd(v3, zs, v13);
  const V v11 = lvx(b, r30 + 48);
  v13 = vmadd(v11, ws, v13);
  v13 = rlimi_1_0(v13, v0);
  o.point = v13;
  uint32_t r27 = 1;
  o.vis = (dotMinReduce(b, lwz(b, 0x829A9390u), v13, radius, writeStatics ? nullptr : &o.touchDummy) & 0xFF) != 0;
  if (!o.vis) r27 = 0;
  const uint32_t lights = lwz(b, 0x82970190u);
  const uint8_t r9b = lbz(b, 0x82970260u);
  const uint32_t n = lwz(b, lights + 720);
  const uint32_t w1A8 = lwz(b, 0x829701A8u);
  if (writeStatics && r9b == 0 && w1A8 == 0) stb(b, 0x82970260u, 1);
  uint32_t r10 = 0x8296FEA0u;
  {
    const uint32_t r11 = lwz(b, r29 + 64);
    if (w1A8 != 0 && r11 <= 31) {
      const uint32_t off = r11 * 20u;
      if (int32_t(lwz(b, off + 0x8296FEB0u)) != -1) r10 = off + 0x8296FEA0u;
    }
  }
  const uint32_t r30m = lwz(b, r10 + 16);
  for (int32_t r31 = int32_t(n - 1); r31 >= 0; --r31) {
    const uint32_t sh = uint32_t(r31) & 0xFF;
    const uint32_t bit = (sh & 0x20) ? 0u : (1u << (sh & 0x1F));
    if ((bit & r30m) == 0) continue;
    const V z = simde_mm_setzero_ps();
    if ((boundsVolume(b, lights, v13, radius, uint32_t(r31), z, z, 0, writeStatics ? nullptr : &o.touchDummy,
                      false) & 0xFF) == 0)
      break;
    r27 |= (sh & 0x20) ? 0u : (2u << (sh & 0x1F));
  }
  o.mask = r27;
  return o;
}

// Returns false (nothing done) for the snapshot-allocating first call: the guest body runs.
// (Context by pointer: reached only from the gated branches of the sub_82305B88 strong def.)
bool clothBoundsNative(PPCContext* pctx, uint8_t* base) {
  PPCContext& ctx = *pctx;
  uint8_t* b = base;
  const uint32_t r29 = ctx.r3.u32;
  if (lwz(b, r29 + 220) == 0) return false;
  const uint32_t r1 = ctx.r1.u32, sp = r1 - 256;
  stw(b, sp, r1);
  ctx.r1.u32 = sp;
  const uint32_t o88 = lwz(b, r29 + 88);
  stw(b, sp + 276, r29);
  const uint32_t r11 = lwz(b, o88 + 12);
  if (r11 == 0) {
    ctx.r1.u32 = r1;
    return true;
  }
  const uint32_t r28 = lwz(b, r11 + 12);
  if (r28 == 0) {
    ctx.r1.u32 = r1;
    return true;
  }
  {  // vtable +28 (r3 = snapshot, r4 = r29[2])
    const uint32_t snap = lwz(b, r29 + 220);
    const uint32_t a4 = lwz(b, r29 + 8);
    const uint32_t vt = lwz(b, snap);
    stw(b, snap + 8, 0);
    stw(b, snap + 4, 0);
    ctx.r3.u64 = snap;
    ctx.r4.u64 = a4;
    PPC_CALL_INDIRECT_FUNC(lwz(b, vt + 28));
  }
  {  // vtable +48 (r3 = snapshot, r4 = r29+128): the world matrix
    const uint32_t snap = lwz(b, r29 + 220);
    ctx.r3.u64 = snap;
    ctx.r4.u64 = r29 + 128;
    PPC_CALL_INDIRECT_FUNC(lwz(b, lwz(b, snap) + 48));
  }
  ctx.fpscr.enableFlushMode();
  stw(b, sp + 96, 1);  // the guest's r27 spill slot
  {
    // r29[4] &= ~0x80 happens between the static flag load and its init; no aliasing
    const uint32_t f16 = lwz(b, r29 + 16);
    stw(b, r29 + 16, f16 & ~0x80u);
  }
  const BoundsResult o = boundsCompute(b, r29, r28, true);
  if (!o.vis) stw(b, r29 + 16, lwz(b, r29 + 16) | 128u);
  {  // vtable +88 (r3 = r29, r4 = r1+160 = the centre)
    const uint32_t fn = lwz(b, lwz(b, r29) + 88);
    stw(b, r29 + 48, o.mask);
    stvx(b, sp + 160, o.point);
    ctx.r3.u64 = r29;
    ctx.r4.u64 = sp + 160;
    PPC_CALL_INDIRECT_FUNC(fn);
  }
  stw(b, r29 + 16, lwz(b, r29 + 16) | 512u);
  ctx.r1.u32 = sp + 256;
  return true;
}

#if SR_ORACLE
void boundsReport() {
  double us[3];
  g_bAb.means(us);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_CLOTHBOUNDS] calls=%llu native=%llu fallback=%llu checked=%llu checkBad=%llu | AB us/call "
              "guest=%.3f native=%.3f",
              (unsigned long long)g_bCalls.load(std::memory_order_relaxed),
              (unsigned long long)g_bNative.load(std::memory_order_relaxed),
              (unsigned long long)g_bFall.load(std::memory_order_relaxed),
              (unsigned long long)g_bChecked.load(std::memory_order_relaxed),
              (unsigned long long)g_bBad.load(std::memory_order_relaxed), us[0], us[1]);
}

// CHECK (the guest calls cannot run twice): the guest body runs for real; then the native
// arithmetic re-runs on the post-call state without writing (the refreshed matrix, the cloth's
// sphere, the lights and statics are what the guest used) and must reproduce the mask the guest
// stored in r29[12], its cull flag and the centre it passed to +88 (still at r1-256+160).
void clothBoundsChecked(PPCContext* pctx, uint8_t* base) {
  PPCContext& ctx = *pctx;
  uint8_t* b = base;
  const uint32_t r29 = ctx.r3.u32, sp = ctx.r1.u32 - 256;
  const bool live = lwz(b, r29 + 220) != 0;
  uint32_t r28 = 0;
  {
    const uint32_t r11 = lwz(b, lwz(b, r29 + 88) + 12);
    if (r11) r28 = lwz(b, r11 + 12);
  }
  __imp__sub_82305B88(ctx, base);
  if (!live || !r28) return;
  const BoundsResult o = boundsCompute(b, r29, r28, false);
  const uint32_t gMask = lwz(b, r29 + 48);
  const bool gCulled = (lwz(b, r29 + 16) & 0x80u) != 0;
  const V gPoint = lvx(b, sp + 160);
  const bool bad = gMask != o.mask || gCulled == o.vis ||
                   simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(simde_mm_castps_si128(gPoint),
                                                               simde_mm_castps_si128(o.point))) != 0xFFFF;
  g_bChecked.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    const uint64_t m = g_bBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_CLOTHBOUNDS] MISMATCH #%llu cloth=%08X mask guest=%08X native=%08X culled guest=%d native=%d "
                  "centre.x guest=%08X native=%08X",
                  (unsigned long long)m, r29, gMask, o.mask, int(gCulled), int(!o.vis), elem(gPoint, 0),
                  elem(o.point, 0));
  }
}

// ---- CHECK mode ---------------------------------------------------------------------------
// Regions are EXACTLY what the function writes (iter-467: an earlier version snapshotted
// count*64 bytes per bucket, past the bucket's end into heap memory other threads own, and its
// restore put back stale allocator headers - a dlmalloc freeze). Restores write only the bytes
// that differ, and a bucket's extent is the record count the guest itself appended.
struct Region {
  const char* name;
  uint32_t idx, va, len, rec;
  bool restore;
  std::vector<uint8_t> pre, guest;
};

bool addRegion(std::vector<Region>& rs, uint8_t* b, const char* name, uint32_t idx, uint32_t va,
               uint32_t len, uint32_t rec, bool restore) {
  if (!len) return true;
  uint8_t* p = gp(b, va);
  if (va + len < va) return false;
  if (sr::kernel::aliasGuestVA(va + len - 1) - sr::kernel::aliasGuestVA(va) != len - 1) return false;
  if (!sr::kernel::hostRangeReadable(p, len)) return false;
  Region r;
  r.name = name;
  r.idx = idx;
  r.va = va;
  r.len = len;
  r.rec = rec;
  r.restore = restore;
  r.pre.assign(p, p + len);
  rs.push_back(std::move(r));
  return true;
}

void putBytes(uint8_t* b, const Region& r, const std::vector<uint8_t>& want) {
  uint8_t* p = gp(b, r.va);
  for (uint32_t k = 0; k < r.len; ++k)
    if (p[k] != want[k]) p[k] = want[k];
}

void checkedCall(PPCContext& ctx, uint8_t* b) {
  const uint32_t a1 = ctx.r3.u32;
  const uint32_t n53 = lwz(b, a1 + 212);
  const uint32_t count = lwz(b, a1 + 152);
  const uint32_t rec = lwz(b, a1 + 232) == 2 ? 32u : 64u;
  std::vector<Region> rs;
  bool ok = true;
  ok &= addRegion(rs, b, "a1", 0, a1, 256, 0, false);  // compared, never restored
  ok &= addRegion(rs, b, "a1+48", 0, a1 + 48, 4, 0, true);
  ok &= addRegion(rs, b, "counters", 0, a1 + 216, 16, 0, true);
  ok &= addRegion(rs, b, "mask", 0, lwz(b, a1 + 204), count, 1, true);
  for (uint32_t i = 0; i < n53 && i < 4; ++i)
    ok &= addRegion(rs, b, "bucket", i, lwz(b, a1 + 236 + 4 * i), count * rec, rec, true);
  ok &= addRegion(rs, b, "statics", 0, 0x82970260u, 0xB0, 0, true);
  if (!ok || n53 > 4) {
    g_unchk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_823055A8(ctx, b);
    return;
  }
  const PPCContext in = ctx;
  __imp__sub_823055A8(ctx, b);
  const PPCContext guestOut = ctx;
  // a bucket's written extent = the records the guest appended to it
  for (auto& r : rs)
    if (std::strcmp(r.name, "bucket") == 0) {
      const uint64_t n = uint64_t(lwz(b, a1 + 216 + 4 * r.idx)) * r.rec;
      if (n < r.len) {
        r.len = uint32_t(n);
        r.pre.resize(r.len);
      }
    }
  for (auto& r : rs) {
    uint8_t* p = gp(b, r.va);
    r.guest.assign(p, p + r.len);
  }
  for (auto& r : rs)
    if (r.restore) putBytes(b, r, r.pre);
  ctx = in;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  const bool ran = runNative(ctx, b);
  const uint64_t nativeR3 = ctx.r3.u64;
  bool bad = false;
  const Region* badR = nullptr;
  uint32_t badOff = 0, diffs = 0;
  uint8_t nb = 0;
  if (ran) {
    if (nativeR3 != guestOut.r3.u64) bad = true;
    for (auto& r : rs) {
      const uint8_t* p = gp(b, r.va);
      for (uint32_t k = 0; k < r.len; ++k)
        if (p[k] != r.guest[k]) {
          if (!badR) {
            badR = &r;
            badOff = k;
            nb = p[k];
          }
          ++diffs;
          bad = true;
        }
    }
  }
  // keep the guest's result
  for (auto& r : rs)
    if (r.restore) putBytes(b, r, r.guest);
  ctx = guestOut;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  if (!ran) {
    g_fallback.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (bad) {
    const uint64_t n = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20) {
      char where[160] = "r3 only";
      if (badR)
        std::snprintf(where, sizeof(where), "%s[%u] va=%08X+%X (elem/rec %u) guest=%02X native=%02X diffs=%u",
                      badR->name, badR->idx, badR->va, badOff, badR->rec ? badOff / badR->rec : 0u,
                      badR->guest[badOff], nb, diffs);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_CLOTHMASK] MISMATCH #%llu %s | r3 guest=%llx native=%llx a1=%08X n53=%u count=%u rec=%u "
                  "ctr=%u,%u,%u",
                  (unsigned long long)n, where, (unsigned long long)guestOut.r3.u64,
                  (unsigned long long)nativeR3, a1, n53, count, rec, lwz(b, a1 + 216), lwz(b, a1 + 220),
                  lwz(b, a1 + 224));
    }
  }
}
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::cloth

// ClothUpdateElementLightMask (NATIVE cloth light mask; see the top of this file).
void sub_823055A8(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::cloth;
#if SR_ORACLE
  if (clothMaskAbOn()) {
    // timing A/B: blocks of 512 calls cycle guest / native / native threaded (THREADS > 0)
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double us[3];
      g_ab.means(us);  // starts the TSC calibration
    }
    const int modes = g_threads > 0 ? 3 : 2;
    const int mode = int((n >> 9) % uint64_t(modes));
    t_parNow = mode == 2;
    const uint64_t t0 = __rdtsc();
    if (mode == 0) {
      __imp__sub_823055A8(ctx, base);
    } else if (runNative(ctx, base)) {
      g_native.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_fallback.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_823055A8(ctx, base);
    }
    g_ab.add(mode, __rdtsc() - t0);
    t_parNow = true;
    if ((n & 0x3FFF) == 0) report();
    return;
  }
  if (clothMaskCheckOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    checkedCall(ctx, base);
    if ((n & 0xFFF) == 0) report();
    return;
  }
#endif  // SR_ORACLE
  if (clothMaskOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runNative(ctx, base)) {
      g_native.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_fallback.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_823055A8(ctx, base);
    }
    if ((n & 0xFFF) == 0) report();
    return;
  }
  __imp__sub_823055A8(ctx, base);
}

// ClothUpdateBoundsAndLights (NATIVE arithmetic, guest calls kept; see clothBoundsNative).
void sub_82305B88(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::cloth;
#if SR_ORACLE
  if (clothBoundsAbOn()) {
    const uint64_t n = g_bCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double t[3];
      g_bAb.means(t);
    }
    const int mode = int((n >> 8) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0 || !clothBoundsNative(&ctx, base)) __imp__sub_82305B88(ctx, base);
    g_bAb.add(mode, __rdtsc() - t0);
    if ((n & 0xFFFF) == 0) boundsReport();
    return;
  }
  if (clothBoundsCheckOn()) {
    const uint64_t n = g_bCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    clothBoundsChecked(&ctx, base);
    if ((n & 0xFFFF) == 0) boundsReport();
    return;
  }
#endif  // SR_ORACLE
  if (clothBoundsOn()) {
    g_bCalls.fetch_add(1, std::memory_order_relaxed);
    if (clothBoundsNative(&ctx, base)) {
      g_bNative.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_bFall.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_82305B88(ctx, base);
    }
    return;
  }
  __imp__sub_82305B88(ctx, base);
}
