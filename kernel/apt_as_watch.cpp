// iter-297c: AS-interpreter watch — ground truth on whether ActionScript RUNS.
//
// sub_82688B90 = THE ActionScript bytecode interpreter (opcode dispatch table
// at 0x82931420, one handler per opcode byte). r3 = AS VM (global 0x829A19B0:
// +0 value-stack count — the metric we mis-modeled as "aptCount"), r4 =
// bytecode ptr, r5 = scope object (clip), r6 = length (-1 = call-function
// mode). MAIN frame 0 carries two type-8 frame actions (the _global.FlowMgr
// bootstrap) that dispatch here via sub_826BE100. This override counts every
// entry and logs the first few (opcode bytes included) so a K-run states
// plainly whether the menu AS executes and how far it gets.
// SR_AS_WATCH=0 disables logging (counting is free).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "wffsm_obs.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>

#include "sr_oracle.hpp"

#if SR_ORACLE
extern "C" unsigned long __stdcall GetCurrentThreadId(void);

extern "C" void __imp__sub_82688B90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826B3998(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826B1040(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82688AC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8267F400(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826A9978(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826BE8C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827657C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821648B8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8224CDC0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::atomic<uint32_t> g_asRuns{0};
std::atomic<uint32_t> g_framesOpened{0};
bool watchEnabled()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_AS_WATCH");
        return !e || e[0] != '0';   // default ON; =0 keeps the A/B
    }();
    return v;
}
// iter-297i DURABLE REGISTER-FRAME FIX (the retirement iter-297e called for).
// Every stock invocation site opens the AS register frame with size 0 and no
// preloads (DAT_829A1624 = frame base, DAT_829A1628 = size). Any top-level
// bytecode whose compiler cached values in registers (StoreRegister /
// type-4 const refs -> *(idx*4 + base)) silently reads/writes OUT-OF-FRAME
// memory: ICONMENU's master timeline script lost onEnterFrame/gFWS/
// iDelayFrames (297e), and its 40 type-8 class-init actions lose the cached
// _global — so _global.FlowMgr (script-globals store DAT_829A1588, golden
// map slot hash&0x7F) never defines and ChangeInterfaceState resolves
// nothing (297i golden+log proof). Fix at THE choke point: when the
// interpreter is entered with an EMPTY frame and a heap scope object, open a
// fresh 8-slot frame (base advanced past the live region, r0/r1 = scope, the
// Xenia-equivalent invocation shape proven by the 297e framed re-run) and
// restore around the call. SR_AS_AUTO_FRAME=0 disables.
// iter-402: FOLDED ONTO THE MASTER GATE. This path FABRICATES an ActionScript
// register frame the game never wrote (size 8 at 0x829A1628, `this` into
// r0/r1), which is forcing by the charter's own definition — and it was
// DEFAULT-ON, i.e. active in the forcing-free default exe, which rule 1
// forbids. The registry review (iter-402) caught it; the audit did not, because
// it treated any SR_* gate as "gated" regardless of the gate's default.
// Now: requires SR_ALLOW_FORCING, and SR_AS_AUTO_FRAME=0 still disables it
// within a forcing-allowed run so the A/B survives.
bool autoFrameEnabled()
{
    if (!sr::kernel::forcingAllowed()) return false;
    static const bool v = [] {
        const char* e = std::getenv("SR_AS_AUTO_FRAME");
        return !e || e[0] != '0';   // sub-gate under the master gate
    }();
    return v;
}
} // namespace

namespace sr::kernel {
uint32_t aptAsRunCount() { return g_asRuns.load(std::memory_order_relaxed); }
} // namespace sr::kernel

PPC_FUNC(sub_82688B90)
{
    const uint32_t n = g_asRuns.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watchEnabled() && (n <= 32 || (n & 0xFF) == 0)) {
        const uint32_t vm = ctx.r3.u32, bc = ctx.r4.u32;
        const uint32_t scope = ctx.r5.u32, len = ctx.r6.u32;
        uint8_t op[8] = {};
        if (bc >= 0x10000u && bc < 0xC0000000u) {
            const uint8_t* h = base + bc;
            for (int i = 0; i < 8; ++i) op[i] = h[i];
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[AS_EXEC] #%u vm=%08X bc=%08X scope=%08X len=%d lr=%08X "
            "ops=%02X %02X %02X %02X %02X %02X %02X %02X",
            n, vm, bc, scope, (int)len, (uint32_t)ctx.lr,
            op[0], op[1], op[2], op[3], op[4], op[5], op[6], op[7]);
    }
    // Auto-frame: empty register frame + heap scope -> open a fresh 8-slot
    // frame with r0/r1 = scope, restore after (protocol of sub_826A5A90).
    uint32_t savedBase = 0, savedSize = 0;
    bool framed = false;
    if (autoFrameEnabled()) {
        const uint32_t scope = ctx.r5.u32;
        // ONLY the raw frame-item dispatch (sub_826BE100 body: type-8 frame
        // actions = the class-init/master-script class). A blanket auto-frame
        // for every size-0 entry FROZE the AS event loop (K-run 2026-07-04:
        // entries stuck at 75, props unresolved) — nested call-function-mode
        // entries manage registers differently and must not be framed.
        const uint32_t lr = (uint32_t)ctx.lr;
        const bool fromFrameItemExec = lr >= 0x826BE100u && lr < 0x826BE8C0u;
        savedBase = sr::kernel::readGuestU32BE(0x829A1624u);
        savedSize = sr::kernel::readGuestU32BE(0x829A1628u);
        if (fromFrameItemExec &&
            savedSize == 0 && savedBase >= 0x10000u && savedBase < 0xC0000000u &&
            scope >= 0x10000u && scope < 0xC0000000u) {
            const uint32_t newBase = savedBase;   // size 0: live region is empty here
            sr::kernel::writeGuestBE32(0x829A1628u, 8u);
            sr::kernel::writeGuestBE32(newBase, scope);        // r0 = this
            sr::kernel::writeGuestBE32(newBase + 4u, scope);   // r1 = this
            for (uint32_t r = 2; r < 8; ++r)
                sr::kernel::writeGuestBE32(newBase + r * 4u, 0);
            framed = true;
            const uint32_t f = g_framesOpened.fetch_add(1, std::memory_order_relaxed) + 1;
            if (watchEnabled() && (f <= 8 || (f & 0x3FF) == 0)) {
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[AS_EXEC] auto-frame #%u opened (base=%08X scope=%08X)",
                    f, newBase, scope);
            }
        }
    }
    __imp__sub_82688B90(ctx, base);
    if (framed) {
        sr::kernel::writeGuestBE32(0x829A1624u, savedBase);
        sr::kernel::writeGuestBE32(0x829A1628u, savedSize);
    }
}

// iter-431: the AS register-frame + event-dispatch ground-truth trio (all
// observe-only, [AS_REG_READ]/[AS_REG_STORE]/[AS_EVT]). The menu's flow logic
// hinges on the type-8 master scripts assigning onEnterFrame/FlowMgr; whether
// they can depends on top-level register reads/stores against the global frame
// (base 829A1624 / size 829A1628, sentinel 829A0AB8), and whether per-frame
// event dispatch ever runs. These three name the truth in one K-run.
PPC_FUNC(sub_82688AC8)
{
    // AsPushConstEntry(vm=r3, entry=r4): entry+4 type word, low7==4 && the
    // defined-bit => REGISTER ref; payload +8 = register index.
    if (watchEnabled()) {
        const uint32_t entry = ctx.r4.u32;
        if (entry >= 0x10000u) {
            const uint32_t tw = sr::kernel::readGuestU32BE(entry + 4u);
            const int type = (int)((tw << 25) >> 25);
            const bool defined = (((~tw) >> 27) & 1u) == 0;
            if (type == 4 && defined) {
                static std::atomic<uint32_t> s_r{0};
                const uint32_t n = s_r.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 64 || (n & 0xFF) == 0) {
                    const uint32_t idx = sr::kernel::readGuestU32BE(entry + 8u);
                    const uint32_t fbase = sr::kernel::readGuestU32BE(0x829A1624u);
                    const uint32_t fsize = sr::kernel::readGuestU32BE(0x829A1628u);
                    const uint32_t slot = (fbase >= 0x10000u)
                        ? sr::kernel::readGuestU32BE(fbase + 4u * idx) : 0;
                    const uint32_t sentinel = sr::kernel::readGuestU32BE(0x829A0AB8u);
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[AS_REG_READ] #%u idx=%u size=%u %s slot=%08X%s lr=%08X",
                        n, idx, fsize, idx >= fsize ? "OUT-OF-FRAME" : "ok",
                        slot, slot == sentinel ? " (sentinel/undefined)" : "",
                        (uint32_t)ctx.lr);
                }
            }
        }
    }
    __imp__sub_82688AC8(ctx, base);
}

PPC_FUNC(sub_8267F400)
{
    if (watchEnabled()) {
        static std::atomic<uint32_t> s_w{0};
        const uint32_t n = s_w.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 64 || (n & 0x3FF) == 0) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AS_REG_STORE] #%u idx=%u size(before)=%u lr=%08X",
                n, ctx.r3.u32, sr::kernel::readGuestU32BE(0x829A1628u),
                (uint32_t)ctx.lr);
        }
    }
    __imp__sub_8267F400(ctx, base);
}

PPC_FUNC(sub_826A9978)
{
    if (watchEnabled()) {
        static std::atomic<uint32_t> s_e{0};
        const uint32_t n = s_e.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 48 || (n & 0x1FF) == 0) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AS_EVT] #%u clip=%08X event=%u lr=%08X",
                n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
        }
    }
    __imp__sub_826A9978(ctx, base);
}

// iter-431: the Apt movie CLOCK ground truth. Boot showed exactly 6 root
// enterFrames in one 376ms window (load-time kicks), then zero event
// dispatch for the rest of the run - the per-frame advance never runs.
// This logs every ClipFrameAdvance call (caller lr + the needs-advance
// flag bit 0x2000000 at clip+12): post-boot callers or their absence name
// the broken per-frame driver.
PPC_FUNC(sub_826BE8C0)
{
    if (watchEnabled()) {
        static std::atomic<uint32_t> s_a{0};
        const uint32_t n = s_a.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 64 || (n & 0x3FF) == 0) {
            const uint32_t clip = ctx.r3.u32;
            const uint32_t f12 = clip >= 0x10000u
                ? sr::kernel::readGuestU32BE(clip + 12u) : 0;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[APT_ADV] #%u clip=%08X needsAdv=%u lr=%08X",
                n, clip, (f12 >> 25) & 1u, (uint32_t)ctx.lr);
        }
    }
    __imp__sub_826BE8C0(ctx, base);
}

// iter-431 ORACLE-DRIVEN: the LUA heartbeat. Live Xenia at the press-start /
// profile / menu era runs luaV_execute (sub_827657C8) continuously — ~150k
// executes across that window — i.e. the DOGMA Lua VM, not ActionScript, is
// what drives the front-end flow (the captured golden call chain for the
// interface-state request ran Lua -> HandleMoveModeCommand -> Request-
// InterfaceState). Our boot runs 373 dostrings and then goes quiet; this
// counter measures whether OUR VM keeps executing per-frame after boot.
// [LUA_HEARTBEAT] cadence: first 4, then every 20000th.
PPC_FUNC(sub_827657C8)
{
    if (watchEnabled()) {
        static std::atomic<uint64_t> s_n{0};
        const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n % 20000ull) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[LUA_HEARTBEAT] luaV_execute #%llu lr=%08X",
                (unsigned long long)n, (uint32_t)ctx.lr);
    }
    __imp__sub_827657C8(ctx, base);
}

// iter-431: [FEQ] — the FSM-state script ENQUEUE. sub_82259630(state, mode)
// (every state enter/update/exit thunk tails into it) does NOT run Lua inline:
// it enqueues a TYPE-1 script item on the FMGameScript module queue via
// sub_821648B8(module=DAT_8296D8CC, payload, 1, 0); the module tick drains the
// queue and runs aaXX(mode). If enqueues flow but scripts never run, the DRAIN
// side is the menu wall. Logs every call (bounded): module, item, type, lr.
PPC_FUNC(sub_821648B8)
{
    const bool w = watchEnabled();
    static std::atomic<uint32_t> s_q{0};
    const uint32_t n = w ? (s_q.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
    const uint32_t mod = ctx.r3.u32, item = ctx.r4.u32, ty = ctx.r5.u32,
                   arg = ctx.r6.u32;
    const uint32_t lr = (uint32_t)ctx.lr;
    __imp__sub_821648B8(ctx, base);
    if (w && (n <= 64 || (n & 0xFF) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FEQ] pcall #%u mod=%08X ref=%08X nargs=%u nres=%u rc=%u lr=%08X",
            n, mod, item, ty, arg, ctx.r3.u32, lr);
}

// (iter-431 note: the transition-taker sub_8278C1C0 is already hooked by the
// rich probe in apt_parse_trace.cpp — it dumps dest-state predicates, script
// handle and content per transition; use that tag.)

// iter-433: [FSMCOND] — the Lua transition predicate sub_8224CDC0(txn).
// Zero nres=1 pcalls in the whole run proved the parked condition never
// even CALLS Lua: the fn returns false early when its script binding chain
// (txn+12 -> obj -> +12 -> holder, ref at holder+4) is null. This logs the
// chain per eval (bounded) — names the null link on the parked transition.
PPC_FUNC(sub_8224CDC0)
{
    // iter-455l [WFFSM]: while the load-completion observer is armed, capture
    // EVERY eval unsampled — dest derivation pre-delegate, the REAL boolean
    // post-delegate. This branch supersedes the sampled [FSMCOND] record for
    // the same call (one delegation either way); gate default OFF.
    if (sr::wffsm::active()) {
        sr::wffsm::condBody(
            ctx, base,
            [](PPCContext& c, uint8_t* b) { __imp__sub_8224CDC0(c, b); },
            sr::wffsm::core());
        return;
    }
    if (watchEnabled()) {
        static std::atomic<uint32_t> s_c{0};
        const uint32_t n = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
        // iter-433d: log the predicate's RETURN too — slot 18 stayed healthy a
        // whole roll and the flow still parked, so either busy is honestly 1
        // at poll time or the boolean dies between pcall and predicate.
        const bool logIt = n <= 32 || (n & 0x1FF) == 0;
        if (logIt) {
            const uint32_t txn = ctx.r3.u32;
            const uint32_t o1 = txn >= 0x10000u
                ? sr::kernel::readGuestU32BE(txn + 12u) : 0;
            const uint32_t o2 = o1 >= 0x10000u
                ? sr::kernel::readGuestU32BE(o1 + 12u) : 0;
            const uint32_t ref = o2 >= 0x10000u
                ? sr::kernel::readGuestU32BE(o2 + 4u) : 0;
            const uint32_t lr = (uint32_t)ctx.lr;
            __imp__sub_8224CDC0(ctx, base);
            // iter-433e: dump the Lua stack around TOS. The pcall wrapper
            // settop-pops the result; the reader does toboolean(L, gettop(L)).
            // Slots (Lua 5.0, 8-byte TObject {tt,val}): live TOS at top-8,
            // popped-result residue at top+0. If the residue is boolean-true
            // while ret=0, the read misses the result slot (codegen off-by-one
            // suspect in gettop/settop/index2adr/toboolean -> L6 difftest).
            const uint32_t mod = sr::kernel::readGuestU32BE(0x8296D8CCu);
            const uint32_t L = mod ? sr::kernel::readGuestU32BE(mod + 12u) : 0;
            const uint32_t topP = L ? sr::kernel::readGuestU32BE(L + 8u) : 0;
            const uint32_t baseP = L ? sr::kernel::readGuestU32BE(L + 12u) : 0;
            uint32_t t0 = 0, v0 = 0, t1 = 0, v1 = 0, tm1 = 0, vm1 = 0;
            if (topP >= 0x10000u) {
                tm1 = sr::kernel::readGuestU32BE(topP - 8u);
                vm1 = sr::kernel::readGuestU32BE(topP - 4u);
                t0 = sr::kernel::readGuestU32BE(topP);
                v0 = sr::kernel::readGuestU32BE(topP + 4u);
                t1 = sr::kernel::readGuestU32BE(topP + 8u);
                v1 = sr::kernel::readGuestU32BE(topP + 12u);
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[FSMCOND] #%u txn=%08X o1=%08X o2=%08X ref=%08X%s -> ret=%u lr=%08X "
                "L=%08X cnt=%d TOS[tt=%u v=%08X] res+0[tt=%u v=%08X] res+8[tt=%u v=%08X] tid=%lu",
                n, txn, o1, o2, ref,
                (!o1 || !o2) ? " NULL-BINDING(false w/o pcall)" : "",
                ctx.r3.u32, lr, L,
                (topP && baseP) ? (int)((topP - baseP) / 8) : -1,
                tm1, vm1, t0, v0, t1, v1,
                (unsigned long)GetCurrentThreadId());
            return;
        }
    }
    __imp__sub_8224CDC0(ctx, base);
}

// (iter-431 note: the state-script runner sub_82259630 is already hooked by
// the iter-357h [FSMA] stateExec probe in cs_timeout_underflow_fix.cpp,
// budget 400 — use that tag.)

// iter-463 charter (burndown): the five iter-297j AS perfect-hash overrides
// (sub_826C2978 / 2790 / 2800 / 2890 / 2900, SR_FIX_AS_HASH) are RETIRED. They
// replaced jump-table switches XenonRecomp once lifted as "// ERROR" returns;
// tools/regen/superman_switch_tables.toml now carries those tables and the
// generated bodies hold the real switch. `difftest --ashash-test` (commit
// 0b74685) ran override vs generated on 10000 inputs each - all equal - so the
// guest's own recompiled code runs again and SR_FIX_AS_HASH is gone.

// iter-297j: name-resolver watch, FILTERED to the exact names on the FlowMgr
// resolve path ("_global" component, "FlowMgr" leaf). Logs scope, result, and
// the strNode length field the builtin matcher consumes. SR_AS_WATCH=0 silences.
PPC_FUNC(sub_826B1040)
{
    uint32_t scope = ctx.r3.u32, holdp = ctx.r4.u32, alt = ctx.r5.u32;
    const uint32_t callerLr = (uint32_t)ctx.lr;
    char nm[20] = {};
    uint32_t nlen = 0;
    if (watchEnabled()) {
        uint32_t strNode = (holdp >= 0x10000u && holdp < 0xC0000000u)
            ? sr::kernel::readGuestU32BE(holdp) : 0;
        if (strNode >= 0x10000u && strNode < 0xC0000000u) {
            nlen = (sr::kernel::readGuestU32BE(strNode) & 0xFFFFu);   // +2 u16 len
            uint8_t* h = sr::kernel::guestVAToHost(strNode + 8u);
            if (h) { for (int i = 0; i < 18 && h[i] >= 0x20 && h[i] < 0x7F; ++i) nm[i] = (char)h[i]; }
        }
    }
    const bool interesting =
        (nm[0] == '_' && std::strcmp(nm, "_global") == 0) ||
        std::strcmp(nm, "FlowMgr") == 0;
    __imp__sub_826B1040(ctx, base);
    if (watchEnabled() && interesting) {
        static std::atomic<uint32_t> s_i{0};
        const uint32_t i = s_i.fetch_add(1, std::memory_order_relaxed) + 1;
        if (i <= 24) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NAME_RESOLVE] #%u '%s' (len16=%u) scope=%08X alt=%08X -> %08X "
                "(1588=%08X) lr=%08X",
                i, nm, nlen, scope, alt, ctx.r3.u32,
                sr::kernel::readGuestU32BE(0x829A1588u), callerLr);
        }
    }
}

#else
// Count real interpreter entries for the existing run metric, without bytecode
// probes or synthetic register frames. All other overrides use guest bodies.
namespace { std::atomic<uint32_t> g_asRuns{0}; }
namespace sr::kernel {
uint32_t aptAsRunCount() { return g_asRuns.load(std::memory_order_relaxed); }
}
extern "C" void __imp__sub_82688B90(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82688B90)
{
    g_asRuns.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82688B90(ctx, base);
}
#endif
