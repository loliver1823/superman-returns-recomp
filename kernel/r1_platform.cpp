// R1 (docs/SPLICE_CONVERSION.md): PLATFORM fixes that the old splice bodies carried, now host
// code. Guest setjmp/longjmp cannot be expressed in recompiled code (see below), so Lua's
// luaD_rawrunprotected is implemented here on the game's own frame layout and memory protocol.

#include "r1_hooks.hpp"
#include "guest_context.hpp"

// =====================================================================================
// Guest setjmp/longjmp for Lua's protected calls (iter-350 platform fix; it was the
// sub_82762400 / sub_827A4270 splices). A guest longjmp must unwind HOST frames: the
// recompiled longjmp can only restore guest registers and return, which sent execution back
// through luaD_throw's noreturn path with the setjmp-time register file (the iter-349
// corruption family). luaD_rawrunprotected below snapshots the PPCContext (= setjmp), runs
// the protected call inside a C++ try and registers its jump buffer; Crt_Longjmp throws
// SrGuestLongjmp for a registered buffer (kernel/alloc_overlap_watch.cpp sub_827A4270).
// =====================================================================================
namespace {
struct SrGuestLongjmp {
    uint32_t bufVA;
    uint32_t code;
};
thread_local uint32_t t_ljBufs[16];
thread_local int t_ljDepth = 0;
}  // namespace

extern "C" bool srGuestLongjmpTryThrow(uint32_t bufVA, uint32_t code) {
    for (int i = t_ljDepth - 1; i >= 0; --i)
        if (t_ljBufs[i] == bufVA)
            throw SrGuestLongjmp{bufVA, code};
    return false;
}

// Crt_Longjmp_sub_827A4270's host half; its override (kernel/alloc_overlap_watch.cpp) calls this
// first. For a buffer that luaD_rawrunprotected or setjmpProtected registered, it does what the
// console's Crt_Longjmp does before restoring the registers (IDA 0x827A4270..0x827A42A8). That is
// _NLG_Notify(jmp_buf LR +0x134, jmp_buf SP +0x90, 0) (sub_827AE4D0, the debugger's non-local-goto
// notification), which stores its three arguments at 0x82946E64 / 0x82946E6C / 0x82946E68 (no
// other code references them). Then SrGuestLongjmp unwinds the host frames to the setjmp site.
// Every registered buffer was filled by the guest setjmp, which zeroes the unwind slot +0x138, so
// the RtlUnwind form never applies. Not reproduced: the body's 80-byte frame push (two words in
// the dead stack below the caller's r1). The generated `bl` does not maintain LR, so its
// saved-LR word could not be the console's anyway. Its other register changes are volatile and
// the restore replaces them. An unregistered buffer returns false, and the generated body runs.
namespace sr::r1 {
bool crtLongjmpThrow(PPCContext& ctx, uint8_t* base) {
    const uint32_t buf = ctx.r3.u32, code = ctx.r4.u32;
    bool registered = false;
    for (int i = t_ljDepth - 1; i >= 0 && !registered; --i)
        registered = t_ljBufs[i] == buf;
    if (!registered)
        return false;
    ctx.r3.u64 = PPC_LOAD_U32(buf + 0x134);
    ctx.r4.u64 = PPC_LOAD_U32(buf + 0x90);
    ctx.r5.u64 = 0;
    ctx.lr = 0x827A42AC;
    sub_827AE4D0(ctx, base);
    srGuestLongjmpTryThrow(buf, code);  // throws
    return false;
}
}  // namespace sr::r1

// luaD_rawrunprotected(L, f, ud) -> status (Lua 5.1 ldo.c), on the game's frame layout: a
// 448-byte frame holding struct lua_longjmp at r1+80 {previous at +0, jmp_buf at +16 (r1+96),
// status at +336 (r1+416)} and the spilled L / f / ud at r1+468 / +476 / +484. luaD_throw
// writes the status into the guest struct before it longjmps, so the guest-visible protocol
// (the L->errorJmp chain at L+80 and the status slot) is the game's own.
PPC_FUNC(sub_82762400) {
    // prologue: save LR and r31, open the frame
    ctx.r12.u64 = ctx.lr;
    PPC_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);
    PPC_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
    {
        const uint32_t sp = ctx.r1.u32 - 448;
        PPC_STORE_U32(sp, ctx.r1.u32);
        ctx.r1.u32 = sp;
    }
    const uint32_t sp = ctx.r1.u32;
    const uint32_t L = ctx.r3.u32;
    // lj.status = 0; lj.previous = L->errorJmp; L->errorJmp = &lj; spill f / ud / L
    ctx.r10.s64 = 0;
    PPC_STORE_U32(sp + 476, ctx.r4.u32);
    ctx.r11.u64 = ctx.r3.u64;
    PPC_STORE_U32(sp + 484, ctx.r5.u32);
    ctx.r9.s64 = ctx.r1.s64 + 80;
    ctx.r3.s64 = ctx.r1.s64 + 96;
    PPC_STORE_U32(sp + 416, ctx.r10.u32);
    ctx.r10.u64 = PPC_LOAD_U32(L + 80);
    PPC_STORE_U32(sp + 468, L);
    PPC_STORE_U32(L + 80, ctx.r9.u32);
    PPC_STORE_U32(sp + 80, ctx.r10.u32);
    {
        const uint32_t bufVA = sp + 96;
        // `bl Crt_Setjmp_sub_827A45E0` (0x8276243C) as on the console: the guest jmp_buf holds the
        // setjmp-time registers, the return address 0x82762440 and SP, which Crt_Longjmp's
        // _NLG_Notify reads (iter-467 parity; until then the buffer was left unwritten)
        ctx.lr = 0x82762440;
        sub_827A45E0(ctx, base);
        PPCContext snap = ctx;  // setjmp: the register file at this point
        if (t_ljDepth < 16)
            t_ljBufs[t_ljDepth++] = bufVA;
        try {
            // the setjmp() == 0 path: status = 0 already; call f(L, ud)
            ctx.r31.u64 = PPC_LOAD_U32(sp + 468);
            ctx.r11.u64 = PPC_LOAD_U32(sp + 476);
            ctx.r3.u64 = ctx.r31.u64;
            ctx.r4.u64 = PPC_LOAD_U32(sp + 484);
            ctx.ctr.u64 = ctx.r11.u64;
            ctx.lr = 0x82762460;
            PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        } catch (const SrGuestLongjmp& lj) {
            if (lj.bufVA != bufVA) {
                if (t_ljDepth > 0)
                    --t_ljDepth;
                throw;
            }
            // the guest longjmp restores f14-f31, r13-r31, r1, LR and CR but not the FPSCR
            // (iter-463): keep the throw-time FPSCR and re-sync the host MXCSR with it
            const auto throwFpscr = ctx.fpscr;
            ctx = snap;
            ctx.fpscr = throwFpscr;
            ctx.fpscr.setcsr(ctx.fpscr.csr);
        }
        if (t_ljDepth > 0)
            --t_ljDepth;
        ctx.r31.u64 = PPC_LOAD_U32(sp + 468);
    }
    // L->errorJmp = lj.previous; return lj.status; epilogue
    ctx.r11.u64 = PPC_LOAD_U32(sp + 80);
    ctx.r3.u64 = PPC_LOAD_U32(sp + 416);
    PPC_STORE_U32(ctx.r31.u32 + 80, ctx.r11.u32);
    ctx.r1.s64 = ctx.r1.s64 + 448;
    ctx.r12.u64 = PPC_LOAD_U32(ctx.r1.u32 + -8);
    ctx.lr = ctx.r12.u64;
    ctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -16);
}

// =====================================================================================
// Guest setjmp for the other setjmp users (iter-467, coordinator decision "option A": the
// luaD_rawrunprotected treatment above, generalised). IDA (default.xex) lists eight
// `bl Crt_Setjmp_sub_827A45E0` sites: luaD_rawrunprotected and the seven in kSetjmpSites below.
// The recompiled longjmp cannot return to a setjmp site: every function is a host function,
// so its final `blr` returns into the THROWER's host frame with the other frame's registers.
// Instead (per function):
//   - a strong def of the function runs its generated body through setjmpProtected(): an
//     activation on a thread-local stack, the body inside a C++ try;
//   - a hook before its `bl Crt_Setjmp` (SrSetjmpArm_X) records the function's own copies of
//     the callee-saved registers the guest jmp_buf holds (r13-r31, f14-f31; CR fields 2-4 -
//     with register localization they are host locals, not ctx fields) and the buffer VA;
//   - Crt_Setjmp's strong def below snapshots the PPCContext (the ctx-resident registers:
//     r1, and r13-r31 as the callers see them) and registers the buffer, as luaD_rawrunprotected
//     does, before the guest setjmp fills the guest jmp_buf as usual;
//   - Crt_Longjmp throws SrGuestLongjmp for a registered buffer (kernel/alloc_overlap_watch.cpp);
//     setjmpProtected() catches it for its own buffer, restores the snapshot (keeping the
//     throw-time FPSCR, as the guest longjmp does) and re-enters the body, whose entry hook
//     (SrSetjmpResume_X) restores the function's own register copies, sets r3 = the longjmp
//     value (0 -> 1) and jumps to the instruction after the `bl Crt_Setjmp`. The function's
//     guest frame is untouched in guest memory, as on the console.
// =====================================================================================
namespace {
struct SetjmpSite {
    uint32_t fn;    // the function that calls setjmp
    uint32_t call;  // its `bl Crt_Setjmp_sub_827A45E0`; it resumes at call + 4
};
constexpr SetjmpSite kSetjmpSites[] = {
    {0x8239A988u, 0x8239A9D8u},  // LoadJPEGImage: setjmp(r1+720); libjpeg error_exit sub_82398A80
    {0x8239B700u, 0x8239B794u},  // ImageLoadPng: setjmp(png_ptr); png_error sub_823EEB90
    {0x823F20F8u, 0x823F2134u},  // png_create_read_struct: setjmp(png_ptr); png_error
    {0x827C1170u, 0x827C125Cu},  // NewBackendCompile: setjmp(ctx+1040); NewBackendCompilerLogError
    {0x827C1438u, 0x827C1534u},  // the HLSL back end's other entry: setjmp(ctx+1040); the same
    {0x82858C90u, 0x82858D00u},  // XGraphics microcode compiler: setjmp(state+16); itself, sub_82857C78
    {0x82859260u, 0x828592C8u},  // its other entry: setjmp(state+16); the checked alloc sub_82857C78
};
constexpr int kSjGp = 19, kSjFp = 18, kSjCr = 3;  // r13-r31, f14-f31, cr2-cr4

struct SetjmpActivation {
    const SetjmpSite* site = nullptr;
    int depthBase = 0;          // t_ljDepth when the activation began
    bool pending = false;       // the arm hook ran; Crt_Setjmp not yet
    bool armed = false;         // buf is registered and snap is valid
    bool resuming = false;      // the next body entry resumes after the setjmp call
    uint32_t buf = 0;
    uint32_t value = 0;         // the longjmp value the setjmp call returns on resume
    uint64_t gp[kSjGp] = {};
    uint64_t fp[kSjFp] = {};
    PPCCRRegister cr[kSjCr] = {};
    PPCContext snap;            // ctx at the setjmp call
};
thread_local SetjmpActivation* t_sjActs[32];
thread_local int t_sjTop = 0;

const SetjmpSite* setjmpSite(uint32_t fn) {
    for (const auto& s : kSetjmpSites)
        if (s.fn == fn)
            return &s;
    return nullptr;
}

// The activation of `fn` on top of this thread's stack (the hooks run in its body, and every
// protected function its body called has returned), or null.
SetjmpActivation* topActivation(uint32_t fn) {
    if (t_sjTop <= 0)
        return nullptr;
    SetjmpActivation* a = t_sjActs[t_sjTop - 1];
    return (a && a->site->fn == fn) ? a : nullptr;
}

void sjArm(uint32_t fn, PPCRegister& r3, PPCRegister* const* gp, PPCRegister* const* fp,
           PPCCRRegister* const* cr) {
    SetjmpActivation* a = topActivation(fn);
    if (!a)
        return;
    for (int i = 0; i < kSjGp; ++i) a->gp[i] = gp[i]->u64;
    for (int i = 0; i < kSjFp; ++i) a->fp[i] = fp[i]->u64;
    for (int i = 0; i < kSjCr; ++i) a->cr[i] = *cr[i];
    a->buf = r3.u32;
    a->pending = true;
}

bool sjResume(uint32_t fn, PPCRegister& r3, PPCRegister* const* gp, PPCRegister* const* fp,
              PPCCRRegister* const* cr) {
    SetjmpActivation* a = topActivation(fn);
    if (!a || !a->resuming)
        return false;
    a->resuming = false;
    for (int i = 0; i < kSjGp; ++i) gp[i]->u64 = a->gp[i];
    for (int i = 0; i < kSjFp; ++i) fp[i]->u64 = a->fp[i];
    for (int i = 0; i < kSjCr; ++i) *cr[i] = a->cr[i];
    r3.u64 = a->value;
    return true;  // -> the instruction after the `bl Crt_Setjmp`
}
}  // namespace

namespace sr::r1 {
void setjmpProtected(PPCContext& ctx, uint8_t* base, uint32_t fn,
                     void (*body)(PPCContext&, uint8_t*)) {
    const SetjmpSite* site = setjmpSite(fn);
    if (!site || t_sjTop >= 32) {
        body(ctx, base);
        return;
    }
    SetjmpActivation act;
    act.site = site;
    act.depthBase = t_ljDepth;
    t_sjActs[t_sjTop++] = &act;
    struct Pop {
        SetjmpActivation& a;
        ~Pop() {
            t_ljDepth = a.depthBase;  // drops this activation's buffer (and nothing below it)
            --t_sjTop;
        }
    } pop{act};
    for (;;) {
        try {
            body(ctx, base);
            return;
        } catch (const SrGuestLongjmp& lj) {
            if (!act.armed || lj.bufVA != act.buf)
                throw;  // an outer setjmp's buffer: Pop unregisters ours on the way out
            // the guest longjmp restores f14-f31, r13-r31, r1, LR and CR but not the FPSCR
            // (iter-463): keep the throw-time FPSCR and re-sync the host MXCSR with it
            const auto throwFpscr = ctx.fpscr;
            ctx = act.snap;
            ctx.fpscr = throwFpscr;
            ctx.fpscr.setcsr(ctx.fpscr.csr);
            act.value = lj.code ? lj.code : 1;  // Crt_Longjmp: a value of 0 returns 1
            act.resuming = true;
            t_ljDepth = act.depthBase + 1;      // the buffer stays valid for another longjmp
        }
    }
}
}  // namespace sr::r1

// Crt_Setjmp_sub_827A45E0(buf): for a protected function's armed call, the PPCContext snapshot
// and the buffer registration (the host half of setjmp); then the guest setjmp as generated.
extern "C" void __imp__sub_827A45E0(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_827A45E0) {
    bool consoleLr = (uint32_t)ctx.lr == 0x82762440u;  // luaD_rawrunprotected's call sets it
    if (t_sjTop > 0) {
        SetjmpActivation* a = t_sjActs[t_sjTop - 1];
        if (a->pending && a->buf == ctx.r3.u32) {
            a->pending = false;
            ctx.lr = a->site->call + 4;  // what the console's `bl` leaves in LR (see below)
            consoleLr = true;
            a->snap = ctx;
            t_ljDepth = a->depthBase;
            if (t_ljDepth < 16) {
                t_ljBufs[t_ljDepth++] = a->buf;
                a->armed = true;
            }
        }
    }
    const uint32_t buf = ctx.r3.u32;
    __imp__sub_827A45E0(ctx, base);
    // The console's setjmp stores LR (`mflr r0`, `stw r0,0x134(r3)`), the return address of the
    // `bl` that called it. Crt_Longjmp's _NLG_Notify passes it on. The generated body models
    // neither: the generated `bl` does not set LR, and its `mflr` emits nothing, so it stores
    // whatever r0 held (the hook pointer it just tested, 0). For the eight callers ctx.lr holds
    // the console's value: the arm path above sets it (call + 4), and luaD_rawrunprotected sets
    // 0x82762440. So the slot gets that value, unless the hook pointer dword_829B2FA8 diverted
    // the call (then the hook filled the buffer).
    if (consoleLr && PPC_LOAD_U32(0x829B2FA8u) == 0)
        PPC_STORE_U32(buf + 0x134, (uint32_t)ctx.lr);
}

// The hooks (tools/regen/superman_regen.toml, two per function). The register lists match the
// toml's `registers`: r3, r13-r31, f14-f31, cr2-cr4.
#define SR_SJ_PARAMS                                                                              \
    PPCRegister &r3, PPCRegister &r13, PPCRegister &r14, PPCRegister &r15, PPCRegister &r16,      \
        PPCRegister &r17, PPCRegister &r18, PPCRegister &r19, PPCRegister &r20, PPCRegister &r21, \
        PPCRegister &r22, PPCRegister &r23, PPCRegister &r24, PPCRegister &r25, PPCRegister &r26, \
        PPCRegister &r27, PPCRegister &r28, PPCRegister &r29, PPCRegister &r30, PPCRegister &r31, \
        PPCRegister &f14, PPCRegister &f15, PPCRegister &f16, PPCRegister &f17, PPCRegister &f18, \
        PPCRegister &f19, PPCRegister &f20, PPCRegister &f21, PPCRegister &f22, PPCRegister &f23, \
        PPCRegister &f24, PPCRegister &f25, PPCRegister &f26, PPCRegister &f27, PPCRegister &f28, \
        PPCRegister &f29, PPCRegister &f30, PPCRegister &f31, PPCCRRegister &cr2,                 \
        PPCCRRegister &cr3, PPCCRRegister &cr4
#define SR_SJ_REFS                                                                                  \
    PPCRegister* const gp[kSjGp] = {&r13, &r14, &r15, &r16, &r17, &r18, &r19, &r20, &r21, &r22,   \
                                    &r23, &r24, &r25, &r26, &r27, &r28, &r29, &r30, &r31};        \
    PPCRegister* const fp[kSjFp] = {&f14, &f15, &f16, &f17, &f18, &f19, &f20, &f21, &f22,         \
                                    &f23, &f24, &f25, &f26, &f27, &f28, &f29, &f30, &f31};        \
    PPCCRRegister* const cr[kSjCr] = {&cr2, &cr3, &cr4}
// The hook functions are written out (not generated by a macro) so scripts/gen_override_manifest.py
// finds each one; their bodies are the shared sjArm / sjResume above.
// LoadJPEGImage (sub_8239A988)
void SrSetjmpArm_8239A988(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x8239A988u, r3, gp, fp, cr); }
bool SrSetjmpResume_8239A988(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x8239A988u, r3, gp, fp, cr); }
// ImageLoadPng (sub_8239B700)
void SrSetjmpArm_8239B700(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x8239B700u, r3, gp, fp, cr); }
bool SrSetjmpResume_8239B700(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x8239B700u, r3, gp, fp, cr); }
// png_create_read_struct (sub_823F20F8)
void SrSetjmpArm_823F20F8(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x823F20F8u, r3, gp, fp, cr); }
bool SrSetjmpResume_823F20F8(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x823F20F8u, r3, gp, fp, cr); }
// NewBackendCompile (sub_827C1170)
void SrSetjmpArm_827C1170(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x827C1170u, r3, gp, fp, cr); }
bool SrSetjmpResume_827C1170(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x827C1170u, r3, gp, fp, cr); }
// the HLSL back end's other compile entry (sub_827C1438)
void SrSetjmpArm_827C1438(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x827C1438u, r3, gp, fp, cr); }
bool SrSetjmpResume_827C1438(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x827C1438u, r3, gp, fp, cr); }
// the XGraphics microcode compiler (sub_82858C90)
void SrSetjmpArm_82858C90(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x82858C90u, r3, gp, fp, cr); }
bool SrSetjmpResume_82858C90(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x82858C90u, r3, gp, fp, cr); }
// the XGraphics microcode compiler (sub_82859260)
void SrSetjmpArm_82859260(SR_SJ_PARAMS) { SR_SJ_REFS; sjArm(0x82859260u, r3, gp, fp, cr); }
bool SrSetjmpResume_82859260(SR_SJ_PARAMS) { SR_SJ_REFS; return sjResume(0x82859260u, r3, gp, fp, cr); }

// The strong defs: each function's generated body runs inside setjmpProtected().
// LoadJPEGImage
extern "C" void __imp__sub_8239A988(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8239A988) { sr::r1::setjmpProtected(ctx, base, 0x8239A988u, __imp__sub_8239A988); }
// ImageLoadPng
extern "C" void __imp__sub_8239B700(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8239B700) { sr::r1::setjmpProtected(ctx, base, 0x8239B700u, __imp__sub_8239B700); }
// png_create_read_struct
extern "C" void __imp__sub_823F20F8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_823F20F8) { sr::r1::setjmpProtected(ctx, base, 0x823F20F8u, __imp__sub_823F20F8); }
// the HLSL back end's other compile entry
extern "C" void __imp__sub_827C1438(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_827C1438) { sr::r1::setjmpProtected(ctx, base, 0x827C1438u, __imp__sub_827C1438); }
// the XGraphics microcode compiler
extern "C" void __imp__sub_82858C90(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82858C90) { sr::r1::setjmpProtected(ctx, base, 0x82858C90u, __imp__sub_82858C90); }
// the XGraphics microcode compiler
extern "C" void __imp__sub_82859260(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82859260) { sr::r1::setjmpProtected(ctx, base, 0x82859260u, __imp__sub_82859260); }
// NewBackendCompile_sub_827C1170's strong def is the [NEWBE] observer in
// kernel/cs_timeout_underflow_fix.cpp; it calls setjmpProtected() itself.
