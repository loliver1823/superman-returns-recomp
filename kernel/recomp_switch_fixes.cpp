// iter-357d — recompiler switch-lowering hole fixes (PLATFORM FIX, recompiler-bug
// class; charter-allowed).
//
// XenonRecomp emitted computed-jump (bctr) switches whose case bodies are
// "// ERROR: 0x<target>" + bare return: the function silently returns with r3
// still holding its INPUT (usually the object pointer). 2,476 such case sites
// exist across guest/ppc (grep '// ERROR: 0x') — the durable fix belongs in the
// regen pipeline (emit real jumps to mid-function labels); this file carries
// kernel strong-def overrides for proven-live instances, Ghidra-verified.
//
// First pair (found iter-357d): the Xenos texture WIDTH/HEIGHT getters. Their
// pointer pass-through fed the texture tiler w=h=low16(descriptor ptr) (0xDA60
// = 55904 → a 12.5 GB tiling loop), wedging FMGameRtam's package kick — the
// wall the iter-220 TEXUP fake-success band-aid had been papering over.
// Real dims for the wedge texture: 128x128 (size dword 0x000FE07F, case 1).
//
// Semantics mirrored from Ghidra FUN_8260a328 / FUN_8260a3d0 (+0x34 byte = type
// nibble; +0x30 dword bits [10:9] = dimension select; +0x24 = packed size).

#include "guest_context.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>

#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"

namespace sr { namespace kernel {
std::atomic<uint64_t> g_sweepThreadToken{0};
} }

namespace {

inline uint32_t rd32(uint32_t va) { return sr::kernel::readGuestU32BE(va); }

inline uint32_t rd8(uint32_t va) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  return h ? *h : 0;
}

}  // namespace

// Texture WIDTH getter (guest sub_8260A328). Generated body's switch cases are
// all "// ERROR" bare returns; leaf tails (sub_8260A368/A388/A39C/A3B0/A3B8)
// compiled fine and match the semantics below.
extern "C" void __imp__sub_8260A328(PPCContext& __restrict ctx, uint8_t* base);
void sub_8260A328(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t obj = ctx.r3.u32;
  const uint32_t size = rd32(obj + 0x24);
  const uint32_t typeNibble = rd8(obj + 0x34) & 0xF;
  uint32_t w;
  if (typeNibble == 1 || typeNibble == 2) {
    w = ((size >> 18) & 0x3FFF) + 1;               // sub_8260A3B8 tail
  } else {
    switch ((rd32(obj + 0x30) >> 9) & 3) {
      case 0: w = size + 1; break;                 // 1D (sub_8260A368 tail)
      case 1:
      case 3: w = (size & 0x1FFF) + 1; break;      // 2D/cube (sub_8260A388)
      case 2: w = (size & 0x7FF) + 1; break;       // 3D (sub_8260A39C)
      default: w = 1; break;                       // sub_8260A3B0
    }
  }
  ctx.r3.u64 = w & 0xFFFF;
}

// Texture HEIGHT getter (guest sub_8260A3D0) — identical hole, mirrored fields.
extern "C" void __imp__sub_8260A3D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8260A3D0(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t obj = ctx.r3.u32;
  const uint32_t size = rd32(obj + 0x24);
  const uint32_t typeNibble = rd8(obj + 0x34) & 0xF;
  uint32_t h;
  if (typeNibble == 1 || typeNibble == 2) {
    h = ((size >> 3) & 0x7FFF) + 1;                // sub_8260A450 tail
  } else {
    switch ((rd32(obj + 0x30) >> 9) & 3) {
      case 1:
      case 3: h = ((size >> 13) & 0x1FFF) + 1; break;
      case 2: h = ((size >> 11) & 0x7FF) + 1; break;
      default: h = 1; break;                       // 1D + out-of-range → 1
    }
  }
  ctx.r3.u64 = h & 0xFFFF;
}

// iter-357i: Lua parser getbinopr (guest sub_8276EAF8) — the SAME switch hole,
// and it broke EVERY script in the game: the token→binop mapper's 243-entry
// bctr table (embedded at 0x8276EB1C, case bodies = "li r3,N; blr" at
// 0x8276EEE8..0x8276EF5F) was fully ERROR'd, so the function returned its
// input token; subexpr (sub_8276EF60) then indexed the operator-priority
// table with a raw token → garbage → luaD_throw status=3 on ALL 200 boot
// FSMA dostrings → no state function defined → 55 "attempt to call a nil
// value" at state pcalls → PlayFMVs/menu chain never started.
//
// Fix emulates the ORIGINAL dispatch using the real table + case bodies that
// are already present in the loaded image — no hand-derived mapping to rot:
//   idx = token - 42; idx > 242 → NOBINOPR(14);
//   target = BE32(0x8276EB1C + idx*4); expect "li r3,imm" → r3 = imm.
extern "C" void __imp__sub_8276EAF8(PPCContext& __restrict ctx, uint8_t* base);
void sub_8276EAF8(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t tok = ctx.r3.u32;
  const uint32_t idx = tok - 42u;
  uint32_t result = 14;  // OPR_NOBINOPR
  uint32_t target = 0, insn = 0;
  if (idx <= 242u) {
    target = rd32(0x8276EB1Cu + idx * 4u);
    insn = rd32(target);
    if ((insn & 0xFFFF0000u) == 0x38600000u)  // li r3, imm
      result = insn & 0xFFFFu;
  }
  // iter-357i diag: keyword operators (tok>=0x100) log so we can see whether
  // `and`/`or` even reach here and what the table yields at runtime.
  static std::atomic<unsigned> s_kw{0};
  if (tok >= 0x100u) {
    unsigned k = s_kw.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 24 || (k & (k - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[GETBINOPR] #%u tok=0x%X target=%08X insn=%08X -> %u", k, tok,
          target, insn, result);
  }
  ctx.r3.u64 = result;
}

// The remaining six holed functions of the Lua VM region (all decompile clean
// in Ghidra — the recompiler alone dropped the jump tables). Semantics are
// mirrored 1:1 from the Ghidra decompiles; all guest accesses big-endian.

namespace {
inline void wr32(uint32_t va, uint32_t v) { sr::kernel::writeGuestBE32(va, v); }
inline void wr8(uint32_t va, uint8_t v) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  if (h) *h = v;
}
}  // namespace

// Lua parser block_follow(token) (guest sub_8276F0C0): true iff token closes a
// block — TK 0x104/0x105/0x106/0x114 (else/elseif/end/until) or 0x11f (EOS).
// Holed, it returned the raw token (truthy for nearly everything) → every
// chunk's body parse ended instantly → "syntax error" on ALL scripts.
extern "C" void __imp__sub_8276F0C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8276F0C0(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t t = ctx.r3.u32;
  ctx.r3.u64 =
      (t == 0x104u || t == 0x105u || t == 0x106u || t == 0x114u || t == 0x11fu)
          ? 1
          : 0;
}

// iter-357i: the writer overrides were briefly bisected OFF while chasing the
// 4/4 wild-read crash — that crash turned out to be the u64 switch-dispatch
// recompiler bug (llex EOZ), not these. With them OFF the GC double-frees
// live objects (mark never links gray objects -> sweep collects reachables;
// [DOGMA_FREE] #4/#6 same ptr+size) and DogmaHeapFree eventually cycles.
// They stay ON: coherent mark+free is required the moment compiles succeed.
#define SR_SWFIX_WRITERS 1

#if SR_SWFIX_WRITERS
// Lua GC gray-list linker A (guest sub_82763518; FUN_82763518 in Ghidra).
// r3 = &gray list head, r4 = GCObject. tt at +4, marked at +5, type-specific
// gclist link written, object pushed onto the list. tt 7 (userdata) walks the
// +8 next chain marking as it goes.
extern "C" void __imp__sub_82763518(PPCContext& __restrict ctx, uint8_t* base);
void sub_82763518(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t list = ctx.r3.u32;
  uint32_t obj = ctx.r4.u32;
  uint32_t idx = (uint32_t)rd8(obj + 4) - 5u;
  wr8(obj + 5, (uint8_t)(rd8(obj + 5) | 1u));
  if (idx > 4u) return;
  for (;;) {
    switch (idx) {
      case 0: wr32(obj + 0x18, rd32(list)); wr32(list, obj); return;
      case 1: wr32(obj + 0x08, rd32(list)); wr32(list, obj); return;
      case 3: wr32(obj + 0x4c, rd32(list)); wr32(list, obj); return;
      case 4: wr32(obj + 0x40, rd32(list)); wr32(list, obj); return;
      default: break;  // idx == 2: walk the +8 chain
    }
    obj = rd32(obj + 8);
    if (rd8(obj + 5) & 0x11u) return;
    wr8(obj + 5, (uint8_t)(rd8(obj + 5) | 1u));
    idx = (uint32_t)rd8(obj + 4) - 5u;
    if (idx > 4u) return;
  }
}

// Lua GC gray-list linker B (guest sub_8276354C) — same family, absolute type
// tags 5..9, entered at the walk step (starts by following the +8 chain).
extern "C" void __imp__sub_8276354C(PPCContext& __restrict ctx, uint8_t* base);
void sub_8276354C(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t list = ctx.r3.u32;
  uint32_t obj = ctx.r4.u32;
  for (;;) {
    obj = rd32(obj + 8);
    if (rd8(obj + 5) & 0x11u) return;
    wr8(obj + 5, (uint8_t)(rd8(obj + 5) | 1u));
    switch (rd8(obj + 4)) {
      case 5: wr32(obj + 0x18, rd32(list)); wr32(list, obj); return;
      case 6: wr32(obj + 0x08, rd32(list)); wr32(list, obj); return;
      case 7: continue;  // userdata: keep walking
      case 8: wr32(obj + 0x4c, rd32(list)); wr32(list, obj); return;
      case 9: wr32(obj + 0x40, rd32(list)); wr32(list, obj); return;
      default: return;
    }
  }
}

// Lua GC freeobj dispatch (guest sub_82763FF8): per-type destructor. Holed,
// nothing was ever freed → heap growth → the stray LUA_ERRMEM throws.
// Callees are intact recomp functions — delegate by tt.
extern "C" void __imp__sub_82760928(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82767C28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82767598(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827603E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827674B8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82763FF8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82763FF8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t obj = ctx.r4.u32;
  switch (rd8(obj + 4)) {
    case 4:  // string: sized free (len at +0xC, +0x11 header)
      ctx.r5.u64 = (uint64_t)rd32(obj + 0xc) + 0x11u;
      ctx.r6.u64 = 0;
      __imp__sub_82760928(ctx, base);
      return;
    case 5: __imp__sub_82767C28(ctx, base); return;   // table
    case 6: __imp__sub_82767598(ctx, base); return;   // closure
    case 7:  // userdata: sized free (+0x10 header)
      ctx.r5.u64 = (uint64_t)rd32(obj + 0xc) + 0x10u;
      ctx.r6.u64 = 0;
      __imp__sub_82760928(ctx, base);
      return;
    case 8: __imp__sub_827603E8(ctx, base); return;   // thread
    case 9: __imp__sub_827674B8(ctx, base); return;   // proto
    case 10:  // upval: fixed-size free (0x14)
      ctx.r5.u64 = 0x14;
      ctx.r6.u64 = 0;
      __imp__sub_82760928(ctx, base);
      return;
    default: return;
  }
}

// luaO_rawequalObj (guest sub_82766AA8): tt mismatch → 0; nil → 1; number →
// float compare; everything else → pointer compare (interned/identity).
extern "C" void __imp__sub_82766AA8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82766AA8(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  const uint32_t ta = rd32(a), tb = rd32(b);
  uint32_t eq;
  if (ta != tb) {
    eq = 0;
  } else if (ta == 0) {
    eq = 1;  // nil
  } else if (ta == 3) {
    float fa, fb;
    uint32_t va = rd32(a + 4), vb = rd32(b + 4);
    __builtin_memcpy(&fa, &va, 4);
    __builtin_memcpy(&fb, &vb, 4);
    eq = (fa == fb) ? 1u : 0u;
  } else {
    eq = (rd32(a + 4) == rd32(b + 4)) ? 1u : 0u;
  }
  ctx.r3.u64 = eq;
}

// iter-357j: the render PASS DISPATCHER (guest sub_827851E8) — the same switch
// hole, and it holds the door to the movies: ALL 29 cases were "// ERROR"
// bare-returns, so in script mode NO pass could execute — including case 0x1C
// 'fmvmovie', the ONLY dispatch site of the FMV render pump Function_822E5100
// (mgr vt[0]) that creates the XMV player / decodes frames / fires movie-end.
// Tails mirrored exactly from the binary (disassembly at 0x8278527C..C0):
//   26 cases -> tail-jump mgr->vt[+0x10](mgr, passId, f1=dt)
//   0x17     -> tail-call Function_82775EF8(mgr) = sub_82775EF8
//   0x1C     -> f1 = *(double*)0x820BD200; tail-jump mgr->vt[0](mgr)
//   0xd,0x1a -> return. default (>0x1C) -> return.
// mgr = *(0x829A9384) (render-layer singleton).
extern "C" void __imp__sub_82775EF8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827851E8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827851E8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t passId = ctx.r4.u32;
  static std::atomic<unsigned> s_fmv{0};
  const uint32_t mgr = rd32(0x829A9384u);
  // iter-398f: always-on dispatcher census — lr names the pass-ISSUER (the
  // per-frame driver that stops at the movie-1 boundary); the last line's
  // timestamp dates the pass stream's death.
  {
    static std::atomic<unsigned> s_pd{0};
    unsigned pd = s_pd.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pd <= 12 || (pd % 500) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PASSD] #%u passId=0x%X lr=%08X",
                  pd, passId, (uint32_t)ctx.lr);
  }
  // iter-362n RENDER-PUMP GAP probe (SR_PASS_TRACE): the fmvmovie pass (0x1c) fires only
  // ONCE though the FMV manager reaches "playing" — so per-frame decode never runs. Log
  // EVERY dispatcher call (passId + global counter) to see whether the render-pass loop
  // runs per-frame and which passIds it issues (is 0x1c ever re-issued? does the loop stop?).
  {
    static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
    if (s_trace) {
      static std::atomic<unsigned> s_all{0};
      unsigned a = s_all.fetch_add(1, std::memory_order_relaxed) + 1;
      if (a <= 120 || a % 500 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[PASS_TRACE] #%u passId=0x%X mgr=%08X lr=%08X",
                    a, passId, mgr, (uint32_t)ctx.lr);
    }
  }
  // iter-463 charter (burndown U1): `mgr == 0 -> return` is a HOST addition -
  // the binary loads mgr and calls through its vtable with no null check
  // (generated __imp__sub_827851E8, loc_8278527C). Master-gated; the default
  // exe follows the binary (a NULL mgr reaches PPC_CALL_INDIRECT_FUNC's
  // named log-and-fail with the call site). The switch itself is the
  // platform reimplementation the allowlist covers.
  if (mgr == 0 && sr::kernel::forcingAllowed()) return;
  switch (passId) {
    case 0xd:
    case 0x1a:
      return;
    case 0x17:
      ctx.r3.u32 = mgr;
      __imp__sub_82775EF8(ctx, base);
      return;
    case 0x1c: {
      unsigned n = s_fmv.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PASS_DISPATCH] fmvmovie #%u mgr=%08X vt0=%08X", n, mgr,
            rd32(rd32(mgr)));
      ctx.r3.u32 = mgr;
      const uint32_t c = rd32(0x820BD200u), c2 = rd32(0x820BD204u);
      const uint64_t bits = ((uint64_t)c << 32) | c2;
      __builtin_memcpy(&ctx.f1.f64, &bits, 8);
      ctx.ctr.u32 = rd32(rd32(mgr));  // vt[0]
      PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      return;
    }
    default:
      if (passId > 0x1cu) return;
      // common tail: vt[+0x10](mgr, passId, f1=dt already live)
      ctx.r3.u32 = mgr;
      ctx.ctr.u32 = rd32(rd32(mgr) + 0x10u);
      PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      return;
  }
}

// iter-357j: XMV decoder GetOutputFormat (guest sub_824797D8) — the same
// switch hole, and it wedges the movie-player handshake: the creator thread
// waits for the decoder workers to negotiate an output format, but every case
// bare-returned so fourcc/bpp were never written. Constants decoded from the
// image's case bodies (table 0x82479804, tails 0x8247983C li r10,0xc /
// 0x82479840 store tail; r3=0 preset at entry):
//   fmt 1 YUY2/16  2 UYVY/16  3 YVYU/16  4 rgb0/24  5 rgb0/16  6 fmt3/16
//   fmt 7 rgb0/32  8 rgb0/8   9 IYUV/12 10 I420/12 11 fmt3/12 12 YV12/12
//   else E_INVALIDARG.
extern "C" void __imp__sub_824797D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824797D8(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t fmt = rd32(ctx.r3.u32 + 0x228u);
  uint32_t fourcc = 0, bpp = 0;
  switch (fmt) {
    case 1: fourcc = 0x32595559u; bpp = 0x10; break;   // YUY2
    case 2: fourcc = 0x59565955u; bpp = 0x10; break;   // UYVY
    case 3: fourcc = 0x55595659u; bpp = 0x10; break;   // YVYU
    case 4: fourcc = 0; bpp = 0x18; break;
    case 5: fourcc = 0; bpp = 0x10; break;
    case 6: fourcc = 3; bpp = 0x10; break;
    case 7: fourcc = 0; bpp = 0x20; break;
    case 8: fourcc = 0; bpp = 0x08; break;
    case 9: fourcc = 0x56555949u; bpp = 0x0c; break;   // IYUV
    case 10: fourcc = 0x30323449u; bpp = 0x0c; break;  // I420
    case 11: fourcc = 3; bpp = 0x0c; break;
    case 12: fourcc = 0x32315659u; bpp = 0x0c; break;  // YV12
    default: ctx.r3.u64 = 0x80070057u; return;         // E_INVALIDARG
  }
  if (ctx.r4.u32) wr32(ctx.r4.u32, fourcc);
  if (ctx.r5.u32) {
    uint8_t* h = sr::kernel::guestVAToHost(ctx.r5.u32);
    if (h) { h[0] = (uint8_t)(bpp >> 8); h[1] = (uint8_t)bpp; }
  }
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[XMVFMT] #%u fmt=%u fourcc=%08X bpp=%u", n, fmt, fourcc, bpp);
  ctx.r3.u64 = 0;
}

// iter-362n RENDER-FRAME-LOOP probe (SR_PASS_TRACE): the render-script pass sweep
// (sub_8277B998, 82 pass-dispatches) runs ONCE. sub_8277B998 is called from the
// readiness gate sub_82776EB0 (line 83, when sub_8277A2D8(target) is true). Instrument
// BOTH: how often is readiness called (per-frame or once?), from where (caller lr = the
// frame loop), does it return ready(1) repeatedly, and does the executor stop firing?
extern "C" void __imp__sub_82776EB0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82776EB0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t a1 = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  // iter-449z: THE POSTFX SCHEDULER PROBE (always-on). This is the master
  // postfx dispatch: TOP GATE walks slot list a1[76..] (count a1[79]) and
  // returns 0 if ANY slot's script (a1[slot+20]) or its object (+12) is
  // missing; else runs the WORLD technique by name (a1[84]: 0=MetropolisTech
  // 1=WarWorldTech 2=TwisterTech 3=QuantumSpeedTech; cached indices
  // a1[102..105]) then the effects. The exposure readback (opcode 0x1E) lives
  // in the world technique, which NEVER runs ([SCRIPT_1E] LOCKSITE=0) — this
  // names which half breaks: gate (badSlot>=0), name lookup (techIdx=-1), or
  // neither (scheduler not even called: zero [POSTFX_SCHED] lines).
  uint32_t nSlots = 0, worldKind = 0xFFFFFFFFu;
  int badSlot = -1;
  int techIdx[4] = {-2, -2, -2, -2};
  if (a1 >= 0x10000u) {
    nSlots = rd32(a1 + 79u * 4u);
    for (uint32_t i = 0; i < nSlots && i < 16u; i++) {
      const uint32_t slotIdx = rd32(a1 + (76u + i) * 4u);
      if ((int32_t)slotIdx < 0) continue;
      const uint32_t scr = rd32(a1 + (slotIdx + 20u) * 4u);
      const uint32_t obj = scr >= 0x10000u ? rd32(scr + 12u) : 0u;
      if (!scr || !obj) { badSlot = (int)i; break; }
    }
    worldKind = rd32(a1 + 84u * 4u);
    for (uint32_t k = 0; k < 4u; k++)
      techIdx[k] = (int)rd32(a1 + (102u + k) * 4u);
  }
  __imp__sub_82776EB0(ctx, base);
  {
    static std::atomic<unsigned> s_pn{0};
    const unsigned n = s_pn.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || n % 512 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[POSTFX_SCHED] #%u mgr=%08X nSlots=%u badSlot=%d worldKind=%u "
          "techIdx[M,W,T,Q]=%d,%d,%d,%d rc=%u lr=%08X",
          n, a1, nSlots, badSlot, worldKind, techIdx[0], techIdx[1],
          techIdx[2], techIdx[3], ctx.r3.u32 & 0xFFu, lr);
  }
  static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[RDY_GATE] #%u sub_82776EB0(a1=%08X) -> ready=%u lr=%08X", n, a1, ctx.r3.u32, lr);
  }
}
// iter-362n: generated-source caller chain for the render sweep:
// sub_8230C038 -> sub_82776038 -> sub_82776EB0. Both wrappers are observation-only
// and gated by SR_PASS_TRACE. They distinguish a missing parent invocation from
// Q's object+100 branch suppressing the sweep.
extern "C" void __imp__sub_82776038(PPCContext& __restrict ctx, uint8_t* base);
void sub_82776038(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
  const uint32_t obj = ctx.r3.u32;
  const uint32_t lr = (uint32_t)ctx.lr;
  const uint32_t active = obj ? rd8(obj + 100) : 0;
  const uint32_t owner = obj ? rd32(obj + 96) : 0;
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SWEEP_Q] #%u sub_82776038(obj=%08X active100=%u owner96=%08X) lr=%08X",
          n, obj, active, owner, lr);
  }
  __imp__sub_82776038(ctx, base);
  if (s_trace) {
    static std::atomic<unsigned> s_retN{0};
    const unsigned n = s_retN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SWEEP_Q_RETURN] #%u sub_82776038(obj=%08X) rc=%08X",
          n, obj, ctx.r3.u32);
  }
}

extern "C" void __imp__sub_8230C038(PPCContext& __restrict ctx, uint8_t* base);
void sub_8230C038(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
  const uint32_t obj = ctx.r3.u32;
  if (s_trace)
    sr::kernel::g_sweepThreadToken.store(
        std::hash<std::thread::id>{}(std::this_thread::get_id()),
        std::memory_order_relaxed);
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SWEEP_PARENT] #%u sub_8230C038(obj=%08X active100=%u) lr=%08X",
          n, ctx.r3.u32, ctx.r3.u32 ? rd8(ctx.r3.u32 + 100) : 0, (uint32_t)ctx.lr);
  }
  __imp__sub_8230C038(ctx, base);
  if (s_trace) {
    static std::atomic<unsigned> s_retN{0};
    const unsigned n = s_retN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SWEEP_PARENT_RETURN] #%u sub_8230C038(obj=%08X) rc=%08X",
          n, obj, ctx.r3.u32);
  }
}

// iter-362n: executor probes below the standalone readiness entry sub_82776EB0. r3 is the
// readiness/render manager; these retain its live address and trace the downstream calls.
namespace sr { namespace kernel { extern std::atomic<uint32_t> g_sweepMgr; } }
extern "C" void __imp__sub_82776850(PPCContext& __restrict ctx, uint8_t* base);
void sub_82776850(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
  if (ctx.r3.u32) sr::kernel::g_sweepMgr.store(ctx.r3.u32, std::memory_order_relaxed);
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0) {
      // BACK-CHAIN WALK: r1 = sub_82776EB0's frame (its callee sub_82776850 not yet prologued).
      // Follow [fp]=back-chain up ~8 frames; dump the LR-save candidates (fp+4 and fp+8, PPC/AIX
      // linkage) at each. sub_82776EB0's return addr into its caller Q = one of these -> identifies Q
      // (the vtable dispatcher / render tick that invokes the sweep once vs per-frame on console).
      auto R = [](uint32_t a){ return (a >= 0x1000u && a < 0xC0000000u) ? sr::kernel::readGuestU32BE(a) : 0u; };
      char bc[220]; int bo = 0; bc[0] = 0;
      uint32_t fp = ctx.r1.u32;
      for (int i = 0; i < 8 && fp >= 0x1000u && fp < 0xC0000000u && bo < 190; ++i) {
        uint32_t nb = R(fp);
        bo += snprintf(bc + bo, sizeof(bc) - bo, "[fp=%08X +4=%08X +8=%08X] ", fp, R(fp + 4), R(fp + 8));
        if (nb <= fp) break;      // back-chain must ascend
        fp = nb;
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[RMGR850] #%u mgr=%08X lr=%08X nSlots=%08X | %s",
          n, ctx.r3.u32, (uint32_t)ctx.lr, R(ctx.r3.u32 + 316), bc);
    }
  }
  __imp__sub_82776850(ctx, base);
}
extern "C" void __imp__sub_8277CB90(PPCContext& __restrict ctx, uint8_t* base);
void sub_8277CB90(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[RMGRCB90] #%u sub_8277CB90(a1=%08X) lr=%08X", n, ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_8277CB90(ctx, base);
}
// iter-398f: name the per-frame render driver — sub_82785868 is the thin
// "render if flags&7 + camera ok" wrapper directly above the sweep chain
// ([RS_EXEC] bt). Its caller lr IS the driver that stops at the movie-1
// boundary. Always-on, capped + every-300th; also logs the a1[2] flags word.
extern "C" void __imp__sub_82785868(PPCContext& __restrict ctx, uint8_t* base);
void sub_82785868(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  const uint32_t flags = a1 ? sr::kernel::readGuestU32BE(a1 + 8u) : 0xEE;
  __imp__sub_82785868(ctx, base);
  if (n <= 12 || (n % 300) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[RDRIVER] #%u sub_82785868(a1=%08X flags=%08X) lr=%08X -> rc=%u",
        n, a1, flags, lr, ctx.r3.u32);
}

extern "C" void __imp__sub_8277B998(PPCContext& __restrict ctx, uint8_t* base);
void sub_8277B998(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32, lr = (uint32_t)ctx.lr;
  // iter-398f: always-on (was SR_PASS_TRACE-gated; that env has an unrelated
  // AV-crashing consumer, 0/6 boots). Capped + every-300th — cheap. This is
  // the DIRECT-call surface for the technique-pass executor; the movie-2
  // wedge = its caller stops at the movie-1 boundary; bt names the chain.
  static const bool s_trace = true;
  if (s_trace) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || n % 300 == 0) {
      // iter-362n: tid (which thread runs the sweep) + guest-stack backtrace (scan r1 for
      // 0x82xxxxxx return addrs) = the full per-frame-vs-once caller chain up to the tick.
      // iter-398g: window widened 0x80 -> 0x300 to reach the frame-walk owner.
      uint32_t r1 = ctx.r1.u32;
      char bt[300]; int bo = 0; bt[0] = 0;
      for (uint32_t o = 0; o < 0x300 && bo < 280; o += 4) {
        uint32_t v = sr::kernel::readGuestU32BE(r1 + o);
        if (v >= 0x82000000u && v < 0x82A00000u)
          bo += snprintf(bt + bo, sizeof(bt) - bo, "%08X ", v);
      }
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[RS_EXEC] #%u sub_8277B998(target=%08X a2=%08X) lr=%08X | bt: %s",
          n, a1, a2, lr, bt);
    }
  }
  __imp__sub_8277B998(ctx, base);
}

// iter-357i diag: luaS_newlstr (sub_827607B0) watch for the keyword strings —
// `and` lexes as a plain identifier on our build, meaning its interned
// TString lost the reserved byte (+6). Log every intern of "and"/"or"/"then":
// VA changes over time = the keyword TString was GC-swept (fixed-mark broken)
// and re-interned bare.
extern "C" void __imp__sub_827607B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_827607B0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t str = ctx.r4.u32, len = ctx.r5.u32;
  bool watch = false;
  char what[8] = {0};
  if (len >= 2 && len <= 4 && str >= 0x1000u && str < 0xC0000000u) {
    const char* p = reinterpret_cast<const char*>(base + str);
    if ((len == 3 && !memcmp(p, "and", 3)) || (len == 2 && !memcmp(p, "or", 2)) ||
        (len == 4 && !memcmp(p, "then", 4))) {
      watch = true;
      memcpy(what, p, len);
    }
  }
  __imp__sub_827607B0(ctx, base);
  if (watch) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t ts = ctx.r3.u32;
    if (n <= 40 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[KW_INTERN] #%u \"%s\" -> ts=%08X marked=%02X reserved=%02X lr=%08X",
          n, what, ts, ts ? rd8(ts + 5) : 0, ts ? rd8(ts + 6) : 0,
          (uint32_t)ctx.lr);
  }
}

// Guest sub_8277AF98 (nearby, same hole): 4-case vector-constant setter —
// slot = *(r3+0xD8)[r4]; copies 1..4 dwords from r5 into slot+0x14.. and
// stores count-1 at slot+0x10.
extern "C" void __imp__sub_8277AF98(PPCContext& __restrict ctx, uint8_t* base);
void sub_8277AF98(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  const uint32_t n = ctx.r6.u32;
  if (n < 1u || n > 4u) return;
  const uint32_t slot = rd32(rd32(ctx.r3.u32 + 0xd8u) + ctx.r4.u32 * 4u);
  const uint32_t src = ctx.r5.u32;
  for (uint32_t i = 0; i < n; ++i)
    wr32(slot + 0x14u + i * 4u, rd32(src + i * 4u));
  wr32(slot + 0x10u, n - 1u);
}
#endif  // SR_SWFIX_WRITERS
