// iter-297f: render-chain watch — where does the frame DIE between the live
// UI logic and the GPU?
//
// Five counters on the mapped chain (ENGINE_MAP §5 "Apt RENDER path"):
//   sub_821808F8  AUI render dispatch (FE tick static callee; gated on
//                 wrapper/hide/state — all read open as of iter-297f)
//   sub_826D1D60  AptExecutor::Render (vt 0x820886F8 slot4, via sub_82179230)
//   sub_8269F118  Apt frame render pass (gate *(ctx+0x18)!=0)
//   sub_82690360  display-tree walk (recursive, HOT — count only)
//   sub_821491A0  buffer-replay consumer (FSGameRenderMain vt slot,
//                 static singleton 0x82911370; renderer DAT_829A9384)
// First hit of each logs loudly; after that, counts print via
// sr::kernel::aptRenderWatchLine() (called from the tick-loop probe).
// SR_RENDER_WATCH=0 disables the first-hit logs (counting is free).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include "native_brackets.hpp"
#include "hdr_adaptation.hpp"
#include "sr_oracle.hpp"

extern "C" void __imp__sub_821808F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8217CB78(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821859A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82179230(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D1D60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8269F118(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82690360(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821491A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D1648(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D17A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D1590(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8269AFD0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82686090(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8268B418(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826ACD18(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826ADA70(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8268CA10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D2028(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820FE518(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8277B0A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820FEBA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82611BC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8261F648(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F3C18(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F3ED8(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::kernel { uint32_t readGuestU32BE(uint32_t va); }

namespace {
std::atomic<uint32_t> g_auiDispatch{0};
std::atomic<uint32_t> g_client{0};
std::atomic<uint32_t> g_composite{0};
std::atomic<uint32_t> g_kicker{0};
std::atomic<uint32_t> g_execRender{0};
std::atomic<uint32_t> g_framePass{0};
std::atomic<uint32_t> g_treeWalk{0};
std::atomic<uint32_t> g_consumer{0};
std::atomic<uint32_t> g_textCb{0};
std::atomic<uint32_t> g_shapeCb{0};
std::atomic<uint32_t> g_importRes{0};
std::atomic<uint32_t> g_importOk{0};
std::atomic<uint32_t> g_childVisit{0};   // sub_82686090: nodes reaching the child block
std::atomic<uint32_t> g_invisSkip{0};    // sub_8268B418: nodes skipped (bit31 clear)
std::atomic<uint32_t> g_leafDraw{0};     // sub_826ACD18: leaf char draw callee
bool watchEnabled()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_RENDER_WATCH");
        return !e || e[0] != '0';
    }();
    return v;
}
void firstHit(std::atomic<uint32_t>& c, const char* name, uint32_t r3)
{
    const uint32_t n = c.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && n == 1)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RENDER_WATCH] FIRST %s (r3=%08X)", name, r3);
}
} // namespace

namespace sr::kernel {
// One-line chain census for the tick-loop probe.
void aptRenderWatchLine(char* out, size_t cap)
{
    snprintf(out, cap,
        "aui=%u client=%u comp=%u kick=%u execR=%u pass=%u walk=%u consume=%u txt=%u shp=%u imp=%u/%u chld=%u inv=%u leaf=%u",
        g_auiDispatch.load(std::memory_order_relaxed),
        g_client.load(std::memory_order_relaxed),
        g_composite.load(std::memory_order_relaxed),
        g_kicker.load(std::memory_order_relaxed),
        g_execRender.load(std::memory_order_relaxed),
        g_framePass.load(std::memory_order_relaxed),
        g_treeWalk.load(std::memory_order_relaxed),
        g_consumer.load(std::memory_order_relaxed), g_textCb.load(std::memory_order_relaxed), g_shapeCb.load(std::memory_order_relaxed), g_importRes.load(std::memory_order_relaxed), g_importOk.load(std::memory_order_relaxed),
        g_childVisit.load(std::memory_order_relaxed),
        g_invisSkip.load(std::memory_order_relaxed),
        g_leafDraw.load(std::memory_order_relaxed));
}
} // namespace sr::kernel

#if SR_ORACLE
PPC_FUNC(sub_821808F8)
{
    firstHit(g_auiDispatch, "AUI-dispatch sub_821808F8", ctx.r3.u32);
    __imp__sub_821808F8(ctx, base);
}
PPC_FUNC(sub_8217CB78)
{
    firstHit(g_client, "render-client sub_8217CB78", ctx.r3.u32);
    __imp__sub_8217CB78(ctx, base);
}
PPC_FUNC(sub_821859A8)
{
    firstHit(g_composite, "screen-composite sub_821859A8", ctx.r3.u32);
    __imp__sub_821859A8(ctx, base);
}
// iter-434d: sub_82179230 self-recurses (bl 0x82179230 at 0x8217B36C, guarded by
// fcmpu-EQUALITY on the recomputed length of the vector at r3+16). A wedge roll
// died 0xC00000FD (32MB host stack) inside this recursion on GameKernel runner 49
// at the movie->FE transition. Depth probe: dump the recursion args + the
// displacement vector at depth thresholds so NaN/garbage vs slow-drift is visible.
PPC_FUNC(sub_82179230)
{
    firstHit(g_kicker, "exec-render kicker sub_82179230", ctx.r3.u32);
    static thread_local uint32_t t_sweepDepth = 0;
    uint32_t d = ++t_sweepDepth;
    if (d == 8 || d == 64 || d == 1024 || d == 8192 || d == 32768) {
        uint32_t r3 = ctx.r3.u32;
        uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, o0 = 0, o1 = 0, o2 = 0;
        if (r3 >= 0x80000000u && r3 < 0xA0000000u) {
            w0 = sr::kernel::readGuestU32BE(r3 + 16);
            w1 = sr::kernel::readGuestU32BE(r3 + 20);
            w2 = sr::kernel::readGuestU32BE(r3 + 24);
            w3 = sr::kernel::readGuestU32BE(r3 + 28);
            o0 = sr::kernel::readGuestU32BE(r3 + 0);
            o1 = sr::kernel::readGuestU32BE(r3 + 4);
            o2 = sr::kernel::readGuestU32BE(r3 + 8);
        }
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[SWEEPREC] depth=%u r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X f1=%g lr=%08X "
            "vec16=%08X %08X %08X %08X hdr=%08X %08X %08X",
            d, r3, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64,
            (uint32_t)ctx.lr, w0, w1, w2, w3, o0, o1, o2);
    }
    __imp__sub_82179230(ctx, base);
    --t_sweepDepth;
}
PPC_FUNC(sub_826D1D60)
{
    firstHit(g_execRender, "AptExecutor::Render sub_826D1D60", ctx.r3.u32);
    __imp__sub_826D1D60(ctx, base);
}
PPC_FUNC(sub_8269F118)
{
    firstHit(g_framePass, "frame-pass sub_8269F118", ctx.r3.u32);
    __imp__sub_8269F118(ctx, base);
}
PPC_FUNC(sub_82690360)
{
    firstHit(g_treeWalk, "tree-walk sub_82690360", ctx.r3.u32);
    // iter-297k: what does the walk actually see? Log the received node's
    // flags/stamp/links for the first few calls and periodically after —
    // names the version the walker selected (vs the committed linked tree).
    {
        static std::atomic<uint32_t> s_wn{0};
        const uint32_t wn = s_wn.fetch_add(1, std::memory_order_relaxed) + 1;
        if (watchEnabled() && (wn <= 8 || (wn & 0x1F) == 0)) {
            const uint32_t nd = ctx.r3.u32;
            const bool ok = nd >= 0x10000u && nd < 0xC0000000u;
            const uint32_t wctx = sr::kernel::readGuestU32BE(0x829A17C0u);
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[WALK_SEE] #%u node=%08X flags=%08X stamp=%08X sib=%08X chld=%08X f74=%08X ctx=%08X slot=%08X",
                wn, nd,
                ok ? sr::kernel::readGuestU32BE(nd + 0x18u) : 0,
                ok ? sr::kernel::readGuestU32BE(nd + 0x20u) : 0,
                ok ? sr::kernel::readGuestU32BE(nd + 0x2cu) : 0,
                ok ? sr::kernel::readGuestU32BE(nd + 0x30u) : 0,
                sr::kernel::readGuestU32BE(0x829A1774u), wctx,
                (wctx >= 0x10000u && wctx < 0xC0000000u)
                    ? sr::kernel::readGuestU32BE(wctx + 0x2cu) : 0);
        }
    }
    __imp__sub_82690360(ctx, base);
}
PPC_FUNC(sub_821491A0)
{
    firstHit(g_consumer, "consumer sub_821491A0 (FSGameRenderMain)", ctx.r3.u32);
    // [FGRENDER] iter-361 draws=0 root: FSGameRenderMain wraps renderer->vt[1]
    // (begin) .. vt[2] (end/present) — these emit the register setup (the 660K
    // reg writes) — and gates the scene/resource render sub_821869A8 behind
    // `byte_829A938C && dword_8296D8C8`. If that gate is false every frame, we
    // get reg writes but no draw work. Log the gate globals + renderer object +
    // the sub_82787520 visibility check inputs, roll-independently (this fn runs
    // every frame). renderer = dword_829A9384; frameObj = dword_8296D8DC.
    if (watchEnabled()) {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 8 || (k & 0x7F) == 0) {
            const uint32_t gateB   = sr::kernel::readGuestU32BE(0x829A938Cu) & 0xFFu; // byte_829A938C
            const uint32_t gateD   = sr::kernel::readGuestU32BE(0x8296D8C8u);         // dword_8296D8C8
            const uint32_t renderer= sr::kernel::readGuestU32BE(0x829A9384u);         // dword_829A9384
            const uint32_t frameObj= sr::kernel::readGuestU32BE(0x8296D8DCu);         // dword_8296D8DC
            const uint32_t visArg  = (frameObj >= 0x10000u && frameObj < 0xC0000000u)
                                        ? sr::kernel::readGuestU32BE(frameObj + 84u) : 0xDEADu;
            const uint32_t rvt     = (renderer >= 0x10000u && renderer < 0xC0000000u)
                                        ? sr::kernel::readGuestU32BE(renderer) : 0;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[FGRENDER] #%u lr=%08X byte829A938C=0x%02X dword8296D8C8=0x%08X (scene-gate=%s) "
                "renderer=0x%08X rvt=0x%08X frameObj=0x%08X visArg(+84)=0x%08X",
                k, (uint32_t)ctx.lr, gateB, gateD, (gateB && gateD) ? "ON" : "OFF",
                renderer, rvt, frameObj, visArg);
        }
    }
    sr::kernel::hdr::RenderTickScope hdrTick(base);
    __imp__sub_821491A0(ctx, base);
}
// iter-297j final link: per-char draw callbacks (cmd43=0 while walk runs).
PPC_FUNC(sub_826D1648)
{
    firstHit(g_textCb, "TEXT draw-cb sub_826D1648", ctx.r3.u32);
    // r3 = per-char import/render object; NULL bails before the ring write
    // (obj->vt[+0x20](obj, params)). Split the count and name the vt target.
    {
        static std::atomic<uint32_t> s_live{0}, s_nul{0};
        const uint32_t io = ctx.r3.u32;
        if (io >= 0x10000u && io < 0xC0000000u) {
            const uint32_t k = s_live.fetch_add(1, std::memory_order_relaxed) + 1;
            if (watchEnabled() && (k <= 4 || (k & 0xFF) == 0)) {
                const uint32_t vt = sr::kernel::readGuestU32BE(io);
                // draw emitter geometry gates: g=*(obj+4) container, its +4
                // count gates the whole emit; +0x10 = vertex stream target,
                // +0xC = texture provider.
                const uint32_t g = sr::kernel::readGuestU32BE(io + 4u);
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[TXT_LIVE] #%u obj=%08X vt=%08X draw=%08X geo=%08X geoN=%08X "
                    "strm=%08X texP=%08X (null-bails %u)",
                    k, io, vt,
                    (vt >= 0x10000u && vt < 0xC0000000u)
                        ? sr::kernel::readGuestU32BE(vt + 0x20u) : 0,
                    g,
                    (g >= 0x10000u && g < 0xC0000000u)
                        ? sr::kernel::readGuestU32BE(g + 4u) : 0xDEADu,
                    sr::kernel::readGuestU32BE(io + 0x10u),
                    sr::kernel::readGuestU32BE(io + 0xCu),
                    s_nul.load(std::memory_order_relaxed));
            }
        } else {
            const uint32_t n = s_nul.fetch_add(1, std::memory_order_relaxed) + 1;
            if (watchEnabled() && n == 1)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[TXT_LIVE] first NULL-bail (r3=%08X)", io);
        }
    }
    __imp__sub_826D1648(ctx, base);
}
PPC_FUNC(sub_826D17A0)
{
    firstHit(g_shapeCb, "SHAPE draw-cb sub_826D17A0", ctx.r3.u32);
    __imp__sub_826D17A0(ctx, base);
}
PPC_FUNC(sub_826D1590)
{
    firstHit(g_importRes, "import-resolver sub_826D1590", ctx.r3.u32);
    __imp__sub_826D1590(ctx, base);
    if (ctx.r3.u32 != 0) {
        const uint32_t k = g_importOk.fetch_add(1, std::memory_order_relaxed) + 1;
        if (watchEnabled() && k == 1)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RENDER_WATCH] FIRST import RESOLVED -> %08X", ctx.r3.u32);
    }
}

// iter-297k: live display-list INSERT (sub_8269AFD0) — every placed character
// (all types) passes through here. Its node-graph commit
// (sub_82698370(*(DAT_829a17bc+0x2c), obj, DAT_829a1770)) silently targets
// garbage when the transient logic-ctx global 0x829A17BC is unpublished — the
// root cause of the childless snapshot graph (walk runs, txt/shp never fire).
// Ring-latch every inserted object so the host commit sweep (SR_NODE_COMMIT,
// splash_hooks.cpp) can rebuild links under a properly published ctx.
namespace sr::kernel {
std::atomic<uint32_t> g_liveInsertRing[256];
std::atomic<uint32_t> g_liveInsertN{0};
} // namespace sr::kernel

PPC_FUNC(sub_82686090)
{
    g_childVisit.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82686090(ctx, base);
}
PPC_FUNC(sub_8268B418)
{
    g_invisSkip.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8268B418(ctx, base);
}
PPC_FUNC(sub_826ACD18)
{
    firstHit(g_leafDraw, "leaf char draw sub_826ACD18", ctx.r3.u32);
    // Gate check: draws only run when char type *r3 == 1, and the final call
    // goes through cb *(0x829A1AD0) (golden = 826D1648). Log both for the
    // first few calls — decides "wrong char obj" vs "cb table not installed".
    // Most calls arrive with r3 = small int (glyph index from sub_826AD410's
    // per-glyph loop — they no-op the type gate). Only pointer-valued calls
    // are the node-leaf path (sub_826ADA70 passes *(node+4)). Sample those.
    {
        const uint32_t co = ctx.r3.u32;
        if (co >= 0x10000u && co < 0xC0000000u) {
            static std::atomic<uint32_t> s_lp{0};
            const uint32_t k = s_lp.fetch_add(1, std::memory_order_relaxed) + 1;
            if (watchEnabled() && (k <= 6 || (k & 0xFF) == 0)) {
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[LEAF_SEE] ptr#%u charObj=%08X type=%08X p8=%08X lr=%08X cb1AD0=%08X",
                    k, co,
                    sr::kernel::readGuestU32BE(co),
                    sr::kernel::readGuestU32BE(co + 0x20u),
                    (uint32_t)ctx.lr,
                    sr::kernel::readGuestU32BE(0x829A1AD0u));
            }
        }
    }
    __imp__sub_826ACD18(ctx, base);
}

// vt-dispatched leaf draws (function-pointer calls always route through the
// dispatch table -> strong-defs, unlike same-TU direct calls).
PPC_FUNC(sub_826ADA70)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && (k <= 6 || (k & 0xFF) == 0)) {
        const uint32_t nd = ctx.r3.u32;
        const bool ok = nd >= 0x10000u && nd < 0xC0000000u;
        const uint32_t co = ok ? sr::kernel::readGuestU32BE(nd + 4u) : 0;
        const bool cok = co >= 0x10000u && co < 0xC0000000u;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[LEAF_VT] #%u node=%08X charObj=%08X charType=%08X defFlags=%04X",
            k, nd, co, cok ? sr::kernel::readGuestU32BE(co) : 0,
            // +0xA u16 = prep flags: 0x8000 gates the glyph texture/geometry
            // build loop inside sub_826ACD18.
            cok ? (sr::kernel::readGuestU32BE(co + 8u) & 0xFFFFu) : 0);
    }
    __imp__sub_826ADA70(ctx, base);
}
PPC_FUNC(sub_8268CA10)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && (k <= 4 || (k & 0xFF) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[SHAPE_VT] #%u r3=%08X", k, ctx.r3.u32);
    __imp__sub_8268CA10(ctx, base);
}

// glyph texture CREATE cb (0x829A1AB4) — fires only from the glyph prep loop
// in sub_826ACD18 (gated on text-def flag 0x8000). Zero here = prep never ran
// = empty import geometry (geo=0) = no vertex emits.
PPC_FUNC(sub_826D2028)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && (k <= 4 || (k & 0xFF) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[TEXCREATE] #%u r3=%08X r4=%08X", k, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_826D2028(ctx, base);
}

PPC_FUNC(sub_8269AFD0)
{
    const uint32_t obj = ctx.r5.u32;
    if (obj >= 0x10000u && obj < 0xC0000000u) {
        const uint32_t n =
            sr::kernel::g_liveInsertN.fetch_add(1, std::memory_order_relaxed);
        if (n < 256)
            sr::kernel::g_liveInsertRing[n].store(obj, std::memory_order_relaxed);
        if (watchEnabled() && (n < 4 || n == 255)) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LIVE_INSERT] #%u obj=%08X prev=%08X 17bc=%08X 1770=%08X lr=%08X%s",
                n + 1, obj, ctx.r4.u32,
                sr::kernel::readGuestU32BE(0x829A17BCu),
                sr::kernel::readGuestU32BE(0x829A1770u),
                (uint32_t)ctx.lr, n == 255 ? " (ring FULL)" : "");
        }
    }
    __imp__sub_8269AFD0(ctx, base);
}

// [TEXSIZE] iter-361: diagnose the render divide-by-zero draws=0 root.
// sub_820FE518 is the texture/surface SIZE calc: v17=(dimW*dimH*a4)>>3 then it
// divides 0x100/v17 and faults when a4 (bpp, = r6) makes v17==0. The format
// decoder sub_820FE370 never yields 0 dims, so a bad a4/a5 from the caller is
// the culprit. Log format(a5=r7)+bpp(a4=r6)+a3+caller for the first calls to
// see which surface format is invalid — pure diagnostic (logs then delegates).
PPC_FUNC(sub_820FE518)
{
  SR_NATIVE_SCOPE(0x820FE518);  // NATIVE N0 (scripts/gen_native_brackets.py)
    static std::atomic<uint32_t> s_n{0};
    const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && (k <= 40 || (k & 0x3FF) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[TEXSIZE] #%u a4_bpp=%u a5_fmt=%u a3=0x%08X a6=%u a7=%u caller_lr=0x%08X",
            k, ctx.r6.u32, ctx.r7.u32, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32,
            (uint32_t)ctx.lr);
    // [FMT0OBJ] When the format (a5=r7) is 0, the texObj lives in the caller's
    // (sub_820FEBA0) stack frame — which sub_820FEBA0 reaches via indirect
    // dispatch, so a PPC_FUNC hook on it never fires. Hunt the stack instead:
    // scan up from r1 for a guest heap pointer whose +32 (a1[8]) format field is
    // 0 and whose +0 is a plausible Xenos GPUTEXTURE type word. The candidate at
    // a consistent stack offset across the storm is the culprit.
    if (watchEnabled() && ctx.r7.u32 == 0) {
        // [BBFMT] one-shot: is the display-format global set? dword_8299AEB0
        // should be 0x18280186 (fmt 6) from sub_82611BC0; the back-buffer texObj
        // pointer is dword_82998868 -> its +32 is the format that's coming out 0.
        static std::atomic<bool> s_bb{false};
        bool expd = false;
        if (s_bb.compare_exchange_strong(expd, true)) {
            const uint32_t fg  = sr::kernel::readGuestU32BE(0x8299AEB0u);
            const uint32_t bbp = sr::kernel::readGuestU32BE(0x82998868u);
            const uint32_t bbf = (bbp >= 0x10000u && bbp < 0xC0000000u)
                                     ? sr::kernel::readGuestU32BE(bbp + 32) : 0xDEADBEEFu;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[BBFMT] dword_8299AEB0=0x%08X backbufPtr(dword_82998868)=0x%08X backbufFmt(+32)=0x%08X",
                fg, bbp, bbf);
        }
        static std::atomic<uint32_t> s_f{0};
        const uint32_t fk = s_f.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fk <= 16) {
            const uint32_t sp = ctx.r1.u32;
            int found = 0;
            for (uint32_t off = 0; off <= 0x400 && found < 3; off += 4) {
                const uint32_t p = sr::kernel::readGuestU32BE(sp + off);
                if (p < 0x10000u || p >= 0xC0000000u) continue;
                const uint32_t w0 = sr::kernel::readGuestU32BE(p + 0);
                const uint32_t w8 = sr::kernel::readGuestU32BE(p + 32);
                const uint32_t nib = w0 & 0xF;
                if ((w8 & 0x3F) == 0 && nib >= 2 && nib <= 4 && (w0 >> 24) != 0) {
                    ++found;
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[FMT0OBJ] #%u sp+0x%X obj=0x%08X w0=0x%08X w6=0x%08X w7=0x%08X "
                        "w8=0x%08X w9=0x%08X w10=0x%08X lr=0x%08X",
                        fk, off, p, w0,
                        sr::kernel::readGuestU32BE(p + 24),
                        sr::kernel::readGuestU32BE(p + 28),
                        w8,
                        sr::kernel::readGuestU32BE(p + 36),
                        sr::kernel::readGuestU32BE(p + 40),
                        (uint32_t)ctx.lr);
                }
            }
        }
    }
    __imp__sub_820FE518(ctx, base);
}

// [APTTEX] 2026-07-19: draws=0 root confirm. sub_8277B0A8 = the Apt texture-
// resource -> GPU-texture loader. a2 (=r4) is the texture-resource descriptor;
// it passes format = *(a2+40) into Texture2DLayoutInit -> the fetch constant.
// The render divide-by-zero storm is fmt=0; hypothesis is *(a2+40)==0. Dump the
// resource fields so we see whether +40 is zero and which resources are bad.
PPC_FUNC(sub_8277B0A8)
{
    const uint32_t a2 = ctx.r4.u32;
    if (watchEnabled() && a2 >= 0x10000u && a2 < 0xC0000000u) {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 64) {
            const uint32_t fmt   = sr::kernel::readGuestU32BE(a2 + 40);
            const uint32_t flag8 = sr::kernel::readGuestU32BE(a2 + 8);
            const uint32_t tex12 = sr::kernel::readGuestU32BE(a2 + 12);
            const uint32_t f24   = sr::kernel::readGuestU32BE(a2 + 24);
            const uint32_t id28  = sr::kernel::readGuestU32BE(a2 + 28);
            const uint32_t dims  = sr::kernel::readGuestU32BE(a2 + 36);
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[APTTEX] #%u res=0x%08X fmt(+40)=0x%08X flag8=0x%02X gputex(+12)=0x%08X "
                "f24=0x%08X id28=0x%08X dims(+36)=0x%08X",
                k, a2, fmt, flag8 & 0xFFu, tex12, f24, id28, dims);
        }
    }
    __imp__sub_8277B0A8(ctx, base);
}

// [FMT0TEX] 2026-07-19: identify the format-0 texObj behind the render
// divide-by-zero storm. sub_820FEBA0 (TextureMipLevelLayout) a1=r3=texObj (a
// Xenos GPUTEXTURE fetch constant); fmt = a1[8]&0x3F (byte offset 32). Log ONLY
// when fmt==0 (a few distinct objects, not the valid-texture flood) — dump the
// object words + caller so we can identify what it is (default/null tex, render
// target, movie tex, ...).
PPC_FUNC(sub_820FEBA0)
{
    const uint32_t obj = ctx.r3.u32;
    if (watchEnabled() && obj >= 0x10000u && obj < 0xC0000000u) {
        const uint32_t w8 = sr::kernel::readGuestU32BE(obj + 32);   // a1[8]
        if ((w8 & 0x3F) == 0) {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
            if (k <= 24)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[FMT0TEX] #%u obj=0x%08X w0=0x%08X w6=0x%08X w7=0x%08X w8=0x%08X "
                    "w9=0x%08X w10=0x%08X w12=0x%08X lr=0x%08X",
                    k, obj,
                    sr::kernel::readGuestU32BE(obj + 0),
                    sr::kernel::readGuestU32BE(obj + 24),
                    sr::kernel::readGuestU32BE(obj + 28),
                    w8,
                    sr::kernel::readGuestU32BE(obj + 36),
                    sr::kernel::readGuestU32BE(obj + 40),
                    sr::kernel::readGuestU32BE(obj + 48),
                    (uint32_t)ctx.lr);
        }
    }
    __imp__sub_820FEBA0(ctx, base);
}

// [VMINIT]/[BBBUILD] 2026-07-19: back-buffer format ordering probe.
// REFUTED (oracle-confirmed 2026-07-19): dword_8299AEB0 ending as 0x1A2201BF is
// NOT corruption. sub_82611BC0 (VMINIT) sets it to 0x18280186 EARLY, but its
// caller sub_822EB148 then builds a 96-byte render-config struct (with the
// hardcoded constants v15[2]=0x1A2201BF, v15[9]=0x1A220197) and memcpy's it over
// the global via sub_8279EC60(&dword_8299AEA8, v15, 96). The golden Xenia dump
// shows the identical 0x1A2201BF/0x1A220197 at 0x8299AEB0/0x8299AECC — this is
// exact console behavior. The watchpoint-arming pause below (SR_VMINIT_PAUSE_SEC)
// is the reusable tool that caught the memcpy writer (Frida HW watchpoint on
// 0x8299AEB0 armed during the hold; lr landed in sub_822EB148 after the memcpy).
// [FLUSHGRIND] iter-361: the common recoverable bad-roll cost. GpuResourceUnlock
// Restore_sub_820F3C18(resource) reads resource[5] (=v9, a packed {startTile<<16
// | endTile}), and when v9 != 0xFFFF0000 (the reset sentinel that SKIPS the
// flush) it calls GpuCacheFlushRange with byte range (endTile-startTile)<<7.
// dcbf is a no-op in our recomp, so the loop is pure overhead; when startTile >
// endTile the range underflows to ~4e9 iterations (~88s grind, then recovers).
// Log the resource + its vtable (type) + the bad packed value + caller ONLY when
// the range would underflow — to identify the resource class and trace where
// resource[5] gets its hi>lo value (the root; masking the flush would be a
// band-aid). Pure diagnostic (logs then delegates).
// [VBCREATE] iter-361: catch the stray writer of the vertex-buffer flush-range
// field vb[5]. D3DVertexBufferCreate_sub_820F3ED8 returns the VB header (vb[5]
// just set to 0xFFFF0000, the skip sentinel). For the specific grind VB (created
// by sub_827869F8, caller_lr in 0x82786A00..0x82786B00), log the VB address and,
// under SR_VBCREATE_PAUSE_SEC, hold here (before sub_827869F8's lock/memcpy/
// unlock run) so a Frida write-watchpoint can be armed on vb+0x14 to name the
// store that corrupts it to 0x3F800000. Fault-free reads only.
// iter-447: this hook OWNS CreateVertexBuffer, so it owns the census too.
// [API_OBS]'s `vbCreate` counter lives in hle_api_observe.cpp and had NO
// increment site anywhere — it read 0 by construction and was quoted as
// evidence that the guest creates no vertex buffers. Notify the owner here so
// the number means something. Whether guest geometry flows through D3D9's
// buffer API at all is load-bearing for the VB/IB slice.
// iter-447 [S6_VBID]: the returned VA goes with it — the probe must compare the
// created buffer against the base draws actually resolve from the fetch
// constant, and for INDEX buffers those provably live in different ranges.
namespace sr { namespace kernel { void noteVertexBufferCreate(uint32_t vbVA); } }

PPC_FUNC(sub_820F3ED8)
{
  SR_NATIVE_SCOPE(0x820F3ED8);  // NATIVE N0 (scripts/gen_native_brackets.py)
    const uint32_t callerLR = (uint32_t)ctx.lr;
    __imp__sub_820F3ED8(ctx, base);
    sr::kernel::noteVertexBufferCreate(ctx.r3.u32);
    const uint32_t vb = ctx.r3.u32;
    if (watchEnabled() && callerLR >= 0x82786A00u && callerLR < 0x82786B00u &&
        vb >= 0x10000u && vb < 0xC0000000u) {
        static std::atomic<bool> s_once{false};
        bool expd = false;
        if (s_once.compare_exchange_strong(expd, true)) {
            const uint32_t v5 = sr::kernel::readGuestU32BE(vb + 0x14u);
            SR_LOG_BOOT(sr::LogSeverity::Error,
                "[VBCREATE] grind-VB header=0x%08X vb[5]=0x%08X — ARM WATCHPOINT on vb+0x14=0x%08X",
                vb, v5, vb + 0x14u);
            const char* p = std::getenv("SR_VBCREATE_PAUSE_SEC");
            const int secs = p ? atoi(p) : 0;
            if (secs > 0) {
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[VBCREATE_PAUSE] holding %d s for watchpoint arm; vb+0x14=0x%08X v5=0x%08X",
                    secs, vb + 0x14u, v5);
                for (int i = 0; i < secs * 10; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const uint32_t nv = sr::kernel::readGuestU32BE(vb + 0x14u);
                    if (nv != v5) {
                        SR_LOG_BOOT(sr::LogSeverity::Error,
                            "[VBCREATE_PAUSE] vb[5] CHANGED during pause 0x%08X -> 0x%08X (i=%d)",
                            v5, nv, i);
                        break;
                    }
                }
                SR_LOG_BOOT(sr::LogSeverity::Info, "[VBCREATE_PAUSE] releasing");
            }
        }
    }
}

PPC_FUNC(sub_820F3C18)
{
    const uint32_t res = ctx.r3.u32;
    if (watchEnabled() && res >= 0x10000u && res < 0xC0000000u) {
        const uint32_t v9 = sr::kernel::readGuestU32BE(res + 0x14u);
        const uint32_t hi = v9 >> 16, lo = v9 & 0xFFFFu;
        if (v9 != 0xFFFF0000u && hi > lo) {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 24)
                SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[FLUSHGRIND] resource=0x%08X vt=0x%08X res[5]=0x%08X "
                    "(startTile=%u > endTile=%u => %d-tile underflow) "
                    "res[6]=0x%08X caller_lr=0x%08X (#%u)",
                    res, sr::kernel::readGuestU32BE(res),
                    v9, hi, lo, (int)(lo - hi),
                    sr::kernel::readGuestU32BE(res + 0x18u),
                    (uint32_t)ctx.lr, n);
        }
    }
    __imp__sub_820F3C18(ctx, base);
}

PPC_FUNC(sub_82611BC0)
{
    __imp__sub_82611BC0(ctx, base);
    if (watchEnabled())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[VMINIT] sub_82611BC0 ran -> dword_8299AEB0=0x%08X (want 0x18280186) "
            "dword_8299AECC=0x%08X",
            sr::kernel::readGuestU32BE(0x8299AEB0u),
            sr::kernel::readGuestU32BE(0x8299AECCu));
    // [VMINIT_PAUSE] iter-361: hold the guest here (format still correct) so a
    // Frida hardware write-watchpoint can be armed on dword_8299AEB0 BEFORE the
    // ~3ms device-init window that clobbers it. SR_VMINIT_PAUSE_SEC seconds.
    {
        const char* p = std::getenv("SR_VMINIT_PAUSE_SEC");
        const int secs = p ? atoi(p) : 0;
        if (secs > 0) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[VMINIT_PAUSE] holding %d s for watchpoint arm; aeb0=0x%08X",
                secs, sr::kernel::readGuestU32BE(0x8299AEB0u));
            for (int i = 0; i < secs * 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const uint32_t v = sr::kernel::readGuestU32BE(0x8299AEB0u);
                if (v != 0x18280186u) {
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[VMINIT_PAUSE] aeb0 CHANGED during pause -> 0x%08X (i=%d)", v, i);
                    break;
                }
            }
            SR_LOG_BOOT(sr::LogSeverity::Info, "[VMINIT_PAUSE] releasing; device-init proceeds now");
        }
    }
}
PPC_FUNC(sub_8261F648)
{
    if (watchEnabled())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[BBBUILD] sub_8261F648 ENTRY dword_8299AEB0=0x%08X dword_8299AECC=0x%08X "
            "backbufPtr=0x%08X",
            sr::kernel::readGuestU32BE(0x8299AEB0u),
            sr::kernel::readGuestU32BE(0x8299AECCu),
            sr::kernel::readGuestU32BE(0x82998868u));
    __imp__sub_8261F648(ctx, base);
    if (watchEnabled())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[BBBUILD] sub_8261F648 DONE  backbuf(0x82986F60)+32=0x%08X",
            sr::kernel::readGuestU32BE(0x82986F60u + 32u));
}

#else
// Preserve real render-entry metrics and native features without probing guest objects.
PPC_FUNC(sub_821808F8)
{
    g_auiDispatch.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_821808F8(ctx, base);
}
PPC_FUNC(sub_8217CB78)
{
    g_client.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8217CB78(ctx, base);
}
PPC_FUNC(sub_821859A8)
{
    g_composite.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_821859A8(ctx, base);
}
PPC_FUNC(sub_82179230)
{
    g_kicker.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82179230(ctx, base);
}
PPC_FUNC(sub_826D1D60)
{
    g_execRender.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826D1D60(ctx, base);
}
PPC_FUNC(sub_8269F118)
{
    g_framePass.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8269F118(ctx, base);
}
PPC_FUNC(sub_82690360)
{
    g_treeWalk.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82690360(ctx, base);
}
PPC_FUNC(sub_821491A0)
{
    g_consumer.fetch_add(1, std::memory_order_relaxed);
    sr::kernel::hdr::RenderTickScope hdrTick(base);
    __imp__sub_821491A0(ctx, base);
}
PPC_FUNC(sub_826D1648)
{
    g_textCb.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826D1648(ctx, base);
}
PPC_FUNC(sub_826D17A0)
{
    g_shapeCb.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826D17A0(ctx, base);
}
PPC_FUNC(sub_826D1590)
{
    g_importRes.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826D1590(ctx, base);
    if (ctx.r3.u32) g_importOk.fetch_add(1, std::memory_order_relaxed);
}
PPC_FUNC(sub_82686090)
{
    g_childVisit.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82686090(ctx, base);
}
PPC_FUNC(sub_8268B418)
{
    g_invisSkip.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8268B418(ctx, base);
}
PPC_FUNC(sub_826ACD18)
{
    g_leafDraw.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826ACD18(ctx, base);
}
namespace sr::kernel { void noteVertexBufferCreate(uint32_t vbVA); }
PPC_FUNC(sub_820F3ED8)
{
    SR_NATIVE_SCOPE(0x820F3ED8);
    __imp__sub_820F3ED8(ctx, base);
    sr::kernel::noteVertexBufferCreate(ctx.r3.u32);
}
PPC_FUNC(sub_820FE518)
{
    SR_NATIVE_SCOPE(0x820FE518);
    __imp__sub_820FE518(ctx, base);
}
#endif
