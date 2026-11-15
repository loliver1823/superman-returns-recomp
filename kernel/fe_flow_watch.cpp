// iter-313: NATURAL FE→FLOW→MENU chain watch, DISPATCH-TABLE EDITION.
//
// The iter-311 strong-def version was linker-flaky (hooks fired in one build,
// silent in the next — lld weak-alias resolution vs the guest lib is order-
// dependent; see iter-312 commit). This version patches the runtime dispatch
// table directly (the registerLatches idiom): save the original PPCFunc*,
// install a logging shim, call through. Immune to link order; intercepts all
// indirect/vt calls (which is how all these targets are reached).
//
// Retail gameconfig truth (text.AST payload, 1372 bytes, verified read):
//   //WorldFlow=PCAWorldFlow   <- commented out: flowXmlLoad(NULL) is NORMAL
//   MainScreen=main            <- THE menu entry: the 0040_MAIN root movie
//   UserInterface=1, IntroMovies=1
// The menu path is the AUI movie gate (iter-193 contract): sub_8214FB78
// requires [[0x8296D874]+144]==1 then client->vt[+20](client, cfg+152).
// Watches: movie gate (with a cfg-object byte dump to compare against
// golden's 0x05B38A30: golden has 'WorldData' text at cfg+0x14 = parsed
// store proof), WorldFlowXML init, SetInterfaceState.
// SR_FLOW_WATCH=0 disables logging.

#include "guest/ppc/ppc_recomp_shared.h"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "../gfx/hle_prov_ring.hpp"
#include "../gfx/resource_census.hpp"
#endif
#include "guest_context.hpp"
#include "wffsm_obs.hpp"
#include "natural_gauges.hpp"  // iter-358: uncapped natural counters
#include "guest_call.hpp"
#include "input_recorder.hpp"
#include "../app/log.hpp"
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <thread>

// Defined in splash_hooks.cpp — the fabricated flow-client VA (0 in natural mode).
namespace sr { namespace kernel { extern std::atomic<uint32_t> g_fabMgrVA; } }

// iter-330: XMsg(0xFA, 0x7001A) -> SetInterfaceState relay. Producer =
// stubs.cpp XMsgStartIORequestEx (publishes payload {area,state}); consumer =
// feSmKickThread below (performs the guest call via GuestCaller).
namespace sr { namespace kernel {
std::atomic<bool>     g_xmsg7001A_pending{false};
std::atomic<uint32_t> g_xmsg7001A_payload{0};
} }

// iter-338: the latched MAIN Apt context (published at parse, cleared after —
// apt_parse_trace.cpp latches it into g_menuCtx). The per-frame Apt logic tick
// sub_826C0888 runs with 0x829A17BC UNPUBLISHED (0/garbage, iter-337) so the
// executor never builds. Re-publish g_menuCtx + drive the logic tick from the
// kick thread as the decisive test: does DEF_EXEC fire (MAIN def built + AS
// runs)? SR_APT_LOGIC_DRIVE=0 disables.
namespace sr { namespace kernel { extern std::atomic<uint32_t> g_menuCtx; } }
// iter-350h: WorldFlowXML-init window flag, shared with the strong-def
// probes in alloc_overlap_watch.cpp (they gate their logging on it).
namespace sr { namespace kernel { std::atomic<int> g_srInFlowInit{0}; } }
namespace sr { namespace kernel { std::atomic<uint32_t> g_sweepMgr{0}; } }  // iter-362n: sweep's render manager (external linkage)

namespace {

bool flowWatchOn()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_FLOW_WATCH");
        return !e || e[0] != '0';
    }();
    return v;
}

}  // namespace
namespace sr { namespace kernel {
// iter-398g/h: runner objects captured by shimKernelLoopRun for the [MOVGATE]
// per-runner election dump and the [CSRUN] runner-CS holder trace (external
// linkage so cs_timeout_underflow_fix.cpp's CS hooks can filter on runner+8).
std::atomic<uint32_t> g_kloopMgrs[4]{};
} }
namespace {
using sr::kernel::g_kloopMgrs;

inline uint32_t rd32(uint8_t* base, uint32_t va)
{
    if (va < 0x10000u || va >= 0xC0000000u) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void rdStr(uint8_t* base, uint32_t va, char* out, size_t cap)
{
    out[0] = 0;
    if (va < 0x10000u || va >= 0xC0000000u) return;
    const uint8_t* p = base + va;
    size_t i = 0;
    for (; i + 1 < cap && p[i]; ++i)
        out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
    out[i] = 0;
}

PPCFunc* s_origD508 = nullptr;   // WorldFlowXML init
PPCFunc* s_origFB78 = nullptr;   // frontend movie gate
PPCFunc* s_orig45D0 = nullptr;   // SetInterfaceState
PPCFunc* s_orig23A0 = nullptr;   // AptDrainCompleted
PPCFunc* s_orig2690 = nullptr;   // AptAuxLoadAptFileByPath
PPCFunc* s_origB748 = nullptr;   // PSM dispatcher (splash/FE state tick)
PPCFunc* s_orig5FE8 = nullptr;   // OnPackageBound
PPCFunc* s_orig91F0 = nullptr;   // RtamPackageManager::CreatePackage

// iter-315b: the movie chain dies at PACKAGE BIND — OnPackageBound(cb, pkg)
// takes the pkg==NULL failure branch (registers the zero-blob record the
// parser later chokes on). Log every bind: cb name (+0xC) + pkg result; and
// every CreatePackage by name. One batch = the full bind census.
void shimOnPackageBound(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && n <= 24) {
        char name[64];
        rdStr(base, rd32(base, ctx.r3.u32 + 0xC), name, sizeof(name));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PKG] OnPackageBound #%u cb=%08X name='%s' pkg=%08X%s lr=%08X",
            n, ctx.r3.u32, name, ctx.r4.u32,
            ctx.r4.u32 ? "" : "  *** NULL BIND -> failure record ***",
            (uint32_t)ctx.lr);
    }
    s_orig5FE8(ctx, base);
}

void shimCreatePackage(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    char name[80];
    rdStr(base, ctx.r4.u32, name, sizeof(name));
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PKG] CreatePackage #%u mgr=%08X name='%s' r5=%08X lr=%08X",
            n, ctx.r3.u32, name, ctx.r5.u32, (uint32_t)ctx.lr);
    s_orig91F0(ctx, base);
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[PKG] CreatePackage #%u rc=%08X", n, ctx.r3.u32);
}
PPCFunc* s_orig2590 = nullptr;   // ActivateUI

// iter-314c: the movie request's natural chain is PSM tick -> splash done ->
// FE transition -> ActivateUI(sub_822A2590, zero static xrefs = vt-dispatched)
// -> movieGate FB78 -> client->vt[+20](cfg+152 'main'). Log whether the PSM
// dispatcher ever ticks and whether ActivateUI ever fires; include the PSM's
// splash counter (+0x230) and done flag (+0x234).
void shimPsmDispatch(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && (n <= 12 || (n % 1000) == 0)) {
        const uint32_t psm = ctx.r3.u32;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PSM] dispatch #%u psm=%08X counter[+230]=%08X done[+234]=%08X mode=%u lr=%08X",
            n, psm, psm ? rd32(base, psm + 0x230u) : 0,
            psm ? rd32(base, psm + 0x234u) : 0, ctx.r4.u32, (uint32_t)ctx.lr);
    }
    s_origB748(ctx, base);
}

void shimActivateUI(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PSM] *** ActivateUI *** #%u r3=%08X r4=%08X lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    s_orig2590(ctx, base);
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[PSM] ActivateUI #%u rc=%08X", n, ctx.r3.u32);
}

// iter-463 (observe-only): [APT_SCREEN] - a per-PATH screen marker. [APT_REQ]
// is capped at 24 lines for the whole run, so every screen opened after the
// first gameplay load (the pause menu, its WARNING popup, the reloaded front
// end after QUIT GAME) went unlogged and scenarios could not wait for them.
// Log a request when its path has been requested at most 16 times, or when
// that path was last logged 30 s or more ago. Requests are
// rare (the uncapped [GAUGE] aptReq reads ~40 per 5-minute run), so a mutex and
// a linear scan over a small table are cheap. seen=K is the per-path request
// ordinal (exact while the path has a table slot); a full table logs every
// request of an untracked path with seen=0. COUNT-UNSAFE (per-path cap +
// re-log interval): for counts read [GAUGE] aptReq.
void logAptScreen(uint32_t n, const char* path, uint32_t lr)
{
    constexpr size_t kScreenSlots = 128;
    struct Slot { char path[96]; uint32_t seen; int64_t lastLogMs; };
    static Slot s_slots[kScreenSlots];
    static size_t s_used = 0;
    static std::mutex s_mu;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t seen = 0;
    bool log = true;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        Slot* s = nullptr;
        for (size_t i = 0; i < s_used; ++i)
            if (std::strcmp(s_slots[i].path, path) == 0) { s = &s_slots[i]; break; }
        if (!s && s_used < kScreenSlots) {
            s = &s_slots[s_used++];
            snprintf(s->path, sizeof(s->path), "%s", path);
            s->seen = 0;
            s->lastLogMs = now;
        }
        if (s) {
            seen = ++s->seen;
            // first 16 per path, then at most one line per path per 30 s
            log = seen <= 16u || (now - s->lastLogMs) >= 30000;
            if (log) s->lastLogMs = now;
        }
    }
    if (log)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[APT_SCREEN] path='%s' seen=%u req=%u lr=%08X", path, seen, n, lr);
}

// iter-314b: the natural movie REQUEST is roll-dependent (fired once in
// iter-313). Log every request with its path so batches show request cadence
// and the movie name.
void shimAptLoadByPath(PPCContext& ctx, uint8_t* base)
{
#if SR_ORACLE
    sr::gfx::memres::noteFetchHudRequest(ctx.r3.u32, base);
#endif
    // iter-358: count FIRST and UNCAPPED. The `n <= 24` below caps the verbose
    // log only — a reader counting [APT_REQ] lines would report the cap as the
    // measurement (the sampling trap, 6th occurrence). Read [GAUGE] aptReq.
    sr::kernel::gaugeBump(sr::kernel::Gauge::AptReq);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-455l provenance ring ARM (observe-only): the ring arms on the EXACT
    // runtime request "screens/hud/main_hud". Read the path UNCONDITIONALLY when
    // the ring is enabled — the n<=24 cap below gates the verbose LOG only, and an
    // arm that silently depended on a log cap would be the sampling trap again.
#if SR_ORACLE
    if (sr::gfx::prov::enabled()) {
        char ppath[96];
        rdStr(base, ctx.r3.u32, ppath, sizeof(ppath));
        sr::gfx::prov::ring().onAptRequest(ppath);
    }
#endif
    // iter-455l [WFFSM] ARM (observe-only): the guest string is read INSIDE
    // the observer's checked reader; an unreadable path refuses once.
    sr::wffsm::onAptRequestVA(ctx.r3.u32, base);
    // iter-462 (observe-only): the input tape's anchor - the start of gameplay
    // (kernel/input_recorder.hpp). Host timer only; nothing reaches the guest.
    if (input_recorder::getMode() != input_recorder::Mode::Off) {
        char apath[96];
        rdStr(base, ctx.r3.u32, apath, sizeof(apath));
        if (std::strcmp(apath, "screens/hud/main_hud") == 0) input_recorder::anchor();
    }
    if (flowWatchOn() && n <= 24) {
        char path[96];
        rdStr(base, ctx.r3.u32, path, sizeof(path));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[APT_REQ] #%u path='%s' holder=%08X lr=%08X",
            n, path, ctx.r4.u32, (uint32_t)ctx.lr);
    }
    if (flowWatchOn()) {
        char spath[96];
        rdStr(base, ctx.r3.u32, spath, sizeof(spath));
        logAptScreen(n, spath, (uint32_t)ctx.lr);
    }
    s_orig2690(ctx, base);
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[APT_REQ] #%u rc=%08X", n, ctx.r3.u32);
}

// iter-314: the natural parser fired with blob=0 — the dead hop is the
// completed-queue entries' file records whose vt[+0x10] get-data returns
// NULL. Dump each drained entry BEFORE the drain consumes it: entry list at
// aptMgr+0x7C (nodes {next, entryPtr}); entry {+4 constRec, +8 aptRec,
// +0xC holder}; log each record's vt, first 8 fields, and any name string.
void shimAptDrain(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t mgr = ctx.r3.u32;
    uint32_t head = mgr ? rd32(base, mgr + 0x7C) : 0;
    // count only NON-EMPTY drains (the drain runs every pump pass; completed
    // entries are rare and are exactly what we need to see).
    const uint32_t n = head ? s_n.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (flowWatchOn() && n && n <= 40) {
        uint32_t node = head;
        int guard = 0;
        while (node && node < 0xC0000000u && guard++ < 8) {
            const uint32_t entry = rd32(base, node + 4);
            if (entry) {
                const uint32_t recC = rd32(base, entry + 4);
                const uint32_t recA = rd32(base, entry + 8);
                char dump[420]; int o = 0;
                o += snprintf(dump + o, sizeof(dump) - o, "aptRec=%08X{vt %08X:", recA, rd32(base, recA));
                for (int i = 1; i <= 8 && o < 300; ++i)
                    o += snprintf(dump + o, sizeof(dump) - o, " %08X", rd32(base, recA + i * 4));
                o += snprintf(dump + o, sizeof(dump) - o, "} constRec=%08X{vt %08X}", recC, rd32(base, recC));
                // name hunt: records/entries often carry a char* in the first
                // fields — print any field that looks like printable text.
                char nm[48] = {0};
                for (int i = 0; i <= 8 && !nm[0]; ++i) {
                    const uint32_t p = rd32(base, entry + i * 4);
                    if (p >= 0x10000u && p < 0xC0000000u) {
                        const uint8_t* s = base + p;
                        if (s[0] >= 0x20 && s[0] < 0x7F && s[1] >= 0x20 && s[1] < 0x7F && s[2] >= 0x20 && s[2] < 0x7F)
                            rdStr(base, p, nm, sizeof(nm));
                    }
                }
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[APT_DRAIN] #%u entry=%08X name?='%s' %s", n, entry, nm, dump);
            }
            node = rd32(base, node);
        }
    }
    s_orig23A0(ctx, base);
}

PPCFunc* s_orig6B80 = nullptr;   // provider name-hash lookup (Function_826E6B80)

// iter-318k: the zero-blob discriminator. Every movie-name registry lookup
// logs name + args + result: a MISS for 'main' while the wrapper is loaded
// = request-vs-registration timing (iter-198p class); a HIT with a null
// data hop = the wrapper get-data path. b5_r0's shot#2 aptCount=1 suggests
// ordering: registration-before-request works.
void shimNameLookup(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    char nm[48];
    rdStr(base, ctx.r4.u32, nm, sizeof(nm));
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    const uint32_t callerLr = (uint32_t)ctx.lr;
    // short names (no path separator) = movie/asset lookups — the rare,
    // critical ones ('main', 'IconMenu'...); path names = mount-era noise.
    bool shortName = nm[0] != 0;
    for (const char* p = nm; *p; ++p)
        if (*p == '\\' || *p == '/') { shortName = false; break; }
    s_orig6B80(ctx, base);
    // iter-461: short names are ~100/s in the city; the first 200 and a sample.
    if (flowWatchOn() && (n <= 30 || (n % 500) == 0 ||
                          (shortName && (n <= 200 || (n % 64) == 0 || sr::kernel::logVerbose()))))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[NAME_LOOKUP] #%u mgr=%08X mgrVt=%08X name='%s'(%08X) r5=%08X -> rc=%08X lr=%08X",
            n, r3, (r3 >= 0x10000u) ? rd32(base, r3) : 0, nm, r4, r5,
            ctx.r3.u32, callerLr);
}

PPCFunc* s_origKLoop = nullptr;  // GameKernelLoop_Run (sub_82720B50, mgr vt[0])

// iter-321b: map kernel-loop threads to their managers. The [KMGR] census
// showed the SIM mgr's clock stamp frozen (its loop never cycles) while
// core/render/sound tick — log tid+mgr at loop entry and exit so the
// sampler tids become attributable, and the missing/stuck sim loop names
// itself.
// iter-322b: no [KSTOP] ever fires yet the sim loop's run byte +0x42 goes
// 0 — a STRAY WRITE (scribble/overrun class; the 4 mgrs sit 0xB0 apart on
// the heap). Poller snapshots the mgr header, logs the exact changed
// bytes when +0x42 flips, and under SR_SIM_KEEP=1 heals +0x40/+0x42 so
// the loop survives — probing both the writer's shape and the FE ladder.
bool simKeepOn()
{
    static const bool v = [] {
        // 2026-07-13 PRIME DIRECTIVE: the sim-mgr heal WRITES guest state =
        // forcing; master gate + explicit opt-in required. Observe-only without.
        const char* e = std::getenv("SR_SIM_KEEP");
        return sr::kernel::forcingAllowed() && e && e[0] == '1';
    }();
    return v;
}

void simMgrGuardThread(uint8_t* base, uint32_t mgr)
{
    uint8_t snap[0x90];
    memcpy(snap, base + mgr, sizeof(snap));
    int heals = 0;
    for (int i = 0; i < 600000 && heals < 200; ++i) {   // 10 min @1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        uint8_t* p = base + mgr;
        if (p[0x42] != 0) {
            if (memcmp(snap, p, sizeof(snap)) != 0) memcpy(snap, p, sizeof(snap));
            continue;
        }
        char diff[256];
        int o = 0;
        for (unsigned k = 0; k < sizeof(snap) && o < 220; ++k)
            if (p[k] != snap[k])
                o += snprintf(diff + o, sizeof(diff) - o, " %02X:%02X>%02X", k, snap[k], p[k]);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[KHEAL] sim mgr %08X run byte cleared — changed:%s%s",
            mgr, o ? diff : " (only 42)", simKeepOn() ? " HEALING" : " (observe only)");
        if (!simKeepOn()) return;
        p[0x40] = 1; p[0x42] = 1;
        ++heals;
        memcpy(snap, p, sizeof(snap));
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[KHEAL] guard done (heals=%d)", heals);
}

// iter-350c: PSM state-update invoker (state vt[0] = sub_822B7370, reached
// only by vt dispatch). The delegate chain (invoker -> FUN_822AB748 ->
// splash advance -> movie queue) never fires; this names the pump caller
// (or proves the state-run is never dispatched).
PPCFunc* s_origPsmInvoke = nullptr;
void shimPsmStateInvoke(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[PSM_INVOKE] #%u state=%08X lr=%08X tid=%lu", n, ctx.r3.u32,
            (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    s_origPsmInvoke(ctx, base);
}

// iter-350b: FeMasterTick call counter. PumpOrTick dispatches it INDIRECTLY
// (*(sysTable+0x14) via PPC_CALL_INDIRECT) so a table patch sees every real
// per-tick invocation - the honest tick count the FMV_SM table-shim missed.
extern "C" void __imp__sub_82776850(PPCContext& __restrict ctx, uint8_t* base);  // sweep executor
PPCFunc* s_origFeTick = nullptr;
void shimFeMasterTick(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FETICK] #%u r3=%08X tid=%lu", n, ctx.r3.u32,
            (unsigned long)GetCurrentThreadId());
    // iter-362n RENDER-GATE probe (SR_PASS_TRACE): the render sweep runs on tick #3 only, so read
    // the render manager (**dword_829A93C8) readiness EVERY tick to see WHAT flips after tick #3.
    // sub_82776EB0(mgr) requires: mgr[79]>0 slots AND each slot's resource(mgr[slot+20])+12 != 0;
    // it also loads tech handles mgr[102..105] (WarWorldTech/etc). Dump those + the divider
    // (mgr[74]=+296 limit, mgr[75]=+300 counter from sub_8277A2D8) across ticks to name the gate.
    static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
    uint32_t mgr = sr::kernel::g_sweepMgr.load(std::memory_order_relaxed);
    if (s_trace && mgr && (n % 60 == 0 || n <= 4)) {
        // sub_82776EB0(mgr) readiness: for each of nSlots (mgr[79]) slots mgr[76+i], the resource
        // res=mgr[slot+20] and res+12 must BOTH be non-zero else readiness returns 0 (sweep skipped).
        uint32_t nSlots = rd32(base, mgr + 79*4);
        char rd[120]; int ro = 0; rd[0] = 0;
        for (uint32_t i = 0; i < (nSlots & 0xFF) && i < 6 && ro < 100; ++i) {
            uint32_t slot = rd32(base, mgr + (76 + i) * 4);
            // iter-398f FAULT-SAFETY: slot is an INDEX; a garbage value made
            // mgr+(slot+20)*4 an in-range-but-unmapped VA -> host AV -> VEH
            // death (SR_PASS_TRACE=1 was 0/4 boots). Bound it.
            if (slot >= 1024u) { ro += snprintf(rd + ro, sizeof(rd) - ro, "s%u[slot=WILD] ", i); continue; }
            uint32_t res  = rd32(base, mgr + (slot + 20u) * 4u);
            uint32_t r12  = (res >= 0x10000u && res < 0xC0000000u) ? rd32(base, res + 12) : 0;
            ro += snprintf(rd + ro, sizeof(rd) - ro, "s%u[slot=%d res=%08X +12=%08X] ", i, (int)slot, res, r12);
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RGATE] tick#%u mgr=%08X nSlots=%d div(+296/+300)=%08X/%08X | %s",
            n, mgr, (int)nSlots, rd32(base, mgr + 296), rd32(base, mgr + 300), rd);
    }
    s_origFeTick(ctx, base);
    // iter-362n CONFIRMING DIAGNOSTIC (SR_FMV_PUMP_SWEEP, master-gated forcing): the oracle proved
    // console runs the render sweep every frame; ours once. The manager (g_sweepMgr) is ready every
    // frame. So DRIVE the sweep per-frame here (call the executor sub_82776850(mgr) after the real
    // tick) and see if the FMV visibly animates (decode count rises, smState 1->2->5, frames advance).
    // If it does, the whole diagnosis is CONFIRMED and the fix direction proven. Forcing (calling a
    // guest fn the current path didn't reach this frame) -> master-gated; delete once the natural
    // per-frame invocation is restored. We're on the guest thread with valid ctx/base; snapshot r3,
    // set r3=mgr, call __imp__sub_82776850, restore.
    static const bool s_pump = [] { const char* e = std::getenv("SR_FMV_PUMP_SWEEP"); return e && e[0] == '1'; }();
    if (s_pump && mgr && sr::kernel::forcingAllowed()) {
        static std::atomic<unsigned> s_enterN{0};
        unsigned enterN = s_enterN.fetch_add(1, std::memory_order_relaxed) + 1;
        if (enterN <= 4 || enterN % 300 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PUMP_SWEEP_ENTER] #%u calling sub_82776850(mgr=%08X)", enterN, mgr);
        auto r3save = ctx.r3;
        ctx.r3.u32 = mgr;
        __imp__sub_82776850(ctx, base);
        ctx.r3 = r3save;
        static std::atomic<unsigned> s_pn{0};
        unsigned pn = s_pn.fetch_add(1, std::memory_order_relaxed) + 1;
        if (pn <= 4 || pn % 300 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Warn, "[PUMP_SWEEP] #%u drove sub_82776850(mgr=%08X)", pn, mgr);
    }
    // iter-398f: [MOVGATE] wall-clock timeline of the movie-2 wedge chain
    // (always-on, observe-only, fault-free via readGuestU32BE). Wedge story:
    // movie-1 END notification lost -> PSM doneWord stays 0 -> render slots
    // never re-register (nSlots@mgr+316 -> 0) -> sweep early-outs -> pump
    // dead -> movie 2 starves at RSM state 2. This poller timestamps each
    // link: log on ANY change (else every 30s). mgr VA logged at spawn for
    // the Dr0 who-writes-nSlots step.
    if (mgr) {
        static std::once_flag s_movGateOnce;
        std::call_once(s_movGateOnce, [mgr] {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[MOVGATE] poller spawned mgr=%08X nSlotsVA=%08X", mgr, mgr + 316u);
            std::thread([mgr] {
                uint32_t last = 0xFFFFFFFFu;
                for (int i = 0; i < 300; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    const uint32_t nSlots = sr::kernel::readGuestU32BE(mgr + 316u);
                    const uint32_t psm = sr::kernel::readGuestU32BE(0x8296EDACu);
                    const uint32_t cnt = psm ? sr::kernel::readGuestU32BE(psm + 0x230u) : 0xEE;
                    const uint32_t done = psm ? sr::kernel::readGuestU32BE(psm + 0x234u) : 0xEE;
                    const uint32_t sm = sr::kernel::readGuestU32BE(0x829761E0u);
                    const uint32_t st = sm ? sr::kernel::readGuestU32BE(sm + 4u) : 0xEE;
                    const uint32_t player = sr::kernel::readGuestU32BE(0x829761D4u);
                    // Readiness per slot (sub_82776EB0 contract): slot idx at
                    // mgr+(76+i)*4; res = mgr[(slot+20)*4]; res+12 must be
                    // non-zero for EVERY slot or the whole sweep is skipped.
                    char sl[128]; int so = 0; sl[0] = 0;
                    uint32_t rHash = 0;
                    for (uint32_t k = 0; k < nSlots && k < 3; ++k) {
                        const uint32_t slot = sr::kernel::readGuestU32BE(mgr + (76u + k) * 4u);
                        uint32_t res = 0, r12 = 0;
                        if (slot < 1024u) {
                            res = sr::kernel::readGuestU32BE(mgr + (slot + 20u) * 4u);
                            if (res >= 0x10000u && res < 0xC0000000u)
                                r12 = sr::kernel::readGuestU32BE(res + 12u);
                        }
                        rHash = rHash * 31u + res + r12;
                        so += snprintf(sl + so, sizeof(sl) - so, "s%u[%u res=%08X +12=%08X] ",
                                       k, slot, res, r12);
                        if (so >= (int)sizeof(sl) - 1) break;
                    }
                    // iter-398g: the FE-tick/render frame ring (**0x829A93C8;
                    // producer = FeMasterTick RenderFrameBegin/End 82787650/
                    // 82787730, consumer = sub_821491A0 acquire 82787520).
                    // Wedge hypothesis: stuck ring config after movie-1 end
                    // (acquire sees +56==+60==-1 forever). Dump the header
                    // {+56 next, +60 cur, +80 writeIdx} + each record's
                    // {stamp.hi, state@+12} (ring array at +96).
                    char rg[160]; rg[0] = 0;
                    {
                        const uint32_t pairs = sr::kernel::readGuestU32BE(0x829A93C8u);
                        const uint32_t fr = pairs ? sr::kernel::readGuestU32BE(pairs) : 0;
                        if (fr >= 0x10000u && fr < 0xC0000000u) {
                            const uint32_t nx = sr::kernel::readGuestU32BE(fr + 56u);
                            const uint32_t cu = sr::kernel::readGuestU32BE(fr + 60u);
                            const uint32_t wi = sr::kernel::readGuestU32BE(fr + 80u);
                            const uint32_t arr = sr::kernel::readGuestU32BE(fr + 96u);
                            int ro2 = snprintf(rg, sizeof(rg),
                                "ring[fr=%08X nx=%d cu=%d wi=%u]", fr, (int)nx,
                                (int)cu, wi);
                            for (uint32_t k = 0; k < 4 && arr >= 0x10000u && arr < 0xC0000000u; ++k) {
                                const uint32_t rec = sr::kernel::readGuestU32BE(arr + 4u * k);
                                if (rec < 0x10000u || rec >= 0xC0000000u) break;
                                ro2 += snprintf(rg + ro2, sizeof(rg) - ro2,
                                    " r%u{%u,st=%u}", k,
                                    sr::kernel::readGuestU32BE(rec),
                                    sr::kernel::readGuestU32BE(rec + 12u));
                                if (ro2 >= (int)sizeof(rg) - 1) break;
                            }
                        }
                    }
                    // iter-398g: per-runner election dump — the wedge = one
                    // runner's KernelLoopRun election going false; these
                    // fields are its only inputs.
                    char rn[220]; int no = 0; rn[0] = 0;
                    for (int k = 0; k < 4; ++k) {
                        const uint32_t r = g_kloopMgrs[k].load(std::memory_order_relaxed);
                        if (!r) break;
                        const uint32_t flags = sr::kernel::readGuestU32BE(r + 0x40u);
                        const uint32_t vt = sr::kernel::readGuestU32BE(r);
                        const uint32_t wf = (vt >= 0x10000u && vt < 0xC0000000u)
                                                ? sr::kernel::readGuestU32BE(vt + 0x20u) : 0;
                        const uint32_t accHi = sr::kernel::readGuestU32BE(r + 0x68u);
                        const uint32_t accLo = sr::kernel::readGuestU32BE(r + 0x6Cu);
                        no += snprintf(rn + no, sizeof(rn) - no,
                                       "R%d{%08X f=%08X wf=%08X acc=%08X%08X} ",
                                       k, r, flags, wf, accHi, accLo);
                        if (no >= (int)sizeof(rn) - 1) break;
                    }
                    const uint32_t sig = nSlots ^ (cnt << 4) ^ (done << 8) ^ (st << 12) ^ player ^ rHash;
                    if (sig != last || i % 15 == 0) {
                        last = sig;
                        SR_LOG_BOOT(sr::LogSeverity::Warn,
                            "[MOVGATE] t=%ds nSlots=%u psmCnt=%08X done=%u fmvState=%u player=%08X | %s| %s| %s",
                            i * 2, nSlots, cnt, done, st, player, sl, rg, rn);
                    }
                }
            }).detach();
        });
    }
}

// iter-398f: [SWEEPCALL]/[RDYCALL] always-on caller census on the sweep
// executor sub_82776850 and ready-gate sub_82776EB0 — BOTH vtable-dispatched
// (strong-defs silent, the iter-362n lesson), so these are PPC_LOOKUP_FUNC
// table shims installed by the watcher thread below. The FMV pump dies with
// 3 healthy slots and open gates, so the sweep's CALLER is what stops at
// the movie-1 boundary; lr names it, timestamps date its death.
PPCFunc* s_origSweepExec = nullptr;   // sub_82776850
void shimSweepExec(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SWEEPCALL] #%u sub_82776850(mgr=%08X) lr=%08X", n, ctx.r3.u32,
            (uint32_t)ctx.lr);
    s_origSweepExec(ctx, base);
}
PPCFunc* s_origRdy2 = nullptr;        // sub_82776EB0
void shimRdy2(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t lr = (uint32_t)ctx.lr;
    s_origRdy2(ctx, base);
    if (n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RDYCALL] #%u sub_82776EB0(a1=%08X) lr=%08X -> rc=%08X", n,
            ctx.r3.u32, lr, ctx.r3.u32);
}
}  // namespace

// iter-398g: THE RUNNER PAUSE/RESUME PAIR — KernelLoopRun's election gate has
// exactly one external control, the paused byte at runner+0x41.
// sub_82720498 = Pause (sets 1, snapshots clock), sub_82720520 = Resume
// (clears). The movie-2 wedge signature (slot2's work never elected, runner
// idle-spins, all downstream gates open) = a Pause at movie-1 end with no
// Resume. Strong-defs = direct-call surface; lr names both parties.
extern "C" void __imp__sub_82720498(PPCContext& __restrict ctx, uint8_t* base);
void sub_82720498(PPCContext& __restrict ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 48)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[KPAUSE] #%u runner=%08X lr=%08X tid=%lu", n, ctx.r3.u32,
            (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    __imp__sub_82720498(ctx, base);
}
extern "C" void __imp__sub_82720520(PPCContext& __restrict ctx, uint8_t* base);
void sub_82720520(PPCContext& __restrict ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 48)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[KRESUME] #%u runner=%08X lr=%08X tid=%lu", n, ctx.r3.u32,
            (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    __imp__sub_82720520(ctx, base);
}

namespace {

// iter-398g: THE RENDER-ENABLE BYTE. mgr(*0x829A9384)+100 gates ALL rendering
// (EnvironmentManager_Update_sub_8230C038 skips both view walks when 0; ctor
// sets 1). sub_82775868 = SetRenderEnabled(this, bool) — vtable-dispatched
// ONLY, so table shim. The movie-2 wedge = someone disables at movie-1 end
// and nobody re-enables; the lr here names both parties.
PPCFunc* s_origRenderEnable = nullptr;   // sub_82775868
void shimRenderEnable(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RENDEN] #%u SetRenderEnabled(mgr=%08X, %u) lr=%08X tid=%lu", n,
            ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
            (unsigned long)GetCurrentThreadId());
    s_origRenderEnable(ctx, base);
}
// iter-398g: count the per-frame render entries themselves — does the render
// SYSTEM tick (sub_821491A0) and/or the view-walk (EnvironmentManager_Update
// sub_8230C038, mgr vt[3]) keep running in the wedge? Power-of-2 sampled.
PPCFunc* s_origRSysTick = nullptr;       // sub_821491A0
void shimRSysTick(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[RSYS] #%u sub_821491A0 lr=%08X tid=%lu",
                    n, (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    s_origRSysTick(ctx, base);
}
PPCFunc* s_origEnvUpd = nullptr;         // sub_8230C038
void shimEnvUpd(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t en = sr::kernel::readGuestU32BE(ctx.r3.u32 + 100u) >> 24;
    if (n <= 4 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[ENVUPD] #%u sub_8230C038(mgr=%08X en=%u) lr=%08X", n,
                    ctx.r3.u32, en, (uint32_t)ctx.lr);
    s_origEnvUpd(ctx, base);
}

PPCFunc* s_origTechExec = nullptr;    // sub_8277B998 — the technique-pass executor
void shimTechExec(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TECHEXEC] #%u sub_8277B998(a1=%08X a2=%08X) lr=%08X", n,
            ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    s_origTechExec(ctx, base);
}
// iter-398f TWO-SURFACE RULE (learned again here): a table shim only sees
// INDIRECT (bctrl) calls; direct `bl` calls bind the weak alias straight to
// __imp__. sub_8277B998 is entered DIRECTLY (this table shim stays silent
// while [FMVPUMP] fires inside the body) — the direct-call surface is the
// existing SR_PASS_TRACE strong-def [RS_EXEC] in recomp_switch_fixes.cpp.

// iter-362n RENDER-DRIVER probe (SR_PASS_TRACE): the Xenia oracle proved the render-pass
// sweep (sub_82776EB0 -> sub_8277B998 -> pass 0x1C -> FMV decode) runs PER-FRAME @30fps on
// console, but ONCE in ours. sub_82776EB0 is VTABLE-dispatched (strong-def override silent),
// so shim its PPC_LOOKUP_FUNC slot to (a) count calls (once vs per-frame) and (b) capture the
// guest caller LR (reliable in our recomp) = the per-frame render driver we must replicate.
PPCFunc* s_origRdyGate = nullptr;   // sub_82776EB0
void shimRenderReadyGate(PPCContext& ctx, uint8_t* base)
{
    static const bool s_trace = [] { const char* e = std::getenv("SR_PASS_TRACE"); return e && e[0] == '1'; }();
    if (s_trace) {
        static std::atomic<unsigned> s_n{0};
        unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16 || n % 300 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[RDY_GATE] #%u sub_82776EB0(a1=%08X) lr=%08X tid=%lu", n, ctx.r3.u32,
                (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    }
    s_origRdyGate(ctx, base);
}

// iter-350e: FE FLOW FSM probes — the boot screen chain is DATA-DRIVEN:
// FSM config XML (states/transition/action; parse cb sub_82293398, strings
// "FSM"/"start_state"/"function") -> action factory Function_82291AD8
// (type 0xf = show-screen cmd, vt 0x82017af4) -> Execute sub_82280760
// (vt[1], indirect) -> sub_82259630 queues the TYPE-1 screen item
// (sub_821648B8(feQ, item, 1, 0, name) — the missing aptCount type-1!) +
// RequestInterfaceState(mod, 0). The screen id lands via client vt[2]
// SetScreen sub_8218EAC8 (+0x584=screen, +0x580=*(screen+0x58)) —
// virtual-only, so the table patch is authoritative. Zero [FE_SETSCREEN]
// = the FE flow FSM never executed its start_state show-screen action.
PPCFunc* s_origShowScr = nullptr;
void shimFeShowScreen(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FE_SHOWSCREEN] #%u state=%08X lr=%08X tid=%lu", n, ctx.r3.u32,
            (uint32_t)ctx.lr, (unsigned long)GetCurrentThreadId());
    s_origShowScr(ctx, base);
}
PPCFunc* s_origSetScr = nullptr;
void shimFeSetScreen(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t scr = ctx.r4.u32;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[FE_SETSCREEN] #%u client=%08X screen=%08X id=%08X lr=%08X",
        n, ctx.r3.u32, scr, scr ? rd32(base, scr + 0x58) : 0, (uint32_t)ctx.lr);
    s_origSetScr(ctx, base);
}
PPCFunc* s_origFsmCb = nullptr;
void shimFsmParseCb(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || (n & (n - 1)) == 0) {
        char el[40];
        rdStr(base, ctx.r4.u32, el, sizeof(el));
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FSM_PARSE] #%u this=%08X el='%s' len=%u lr=%08X",
            n, ctx.r3.u32, el, ctx.r5.u32, (uint32_t)ctx.lr);
    }
    s_origFsmCb(ctx, base);
}
PPCFunc* s_origFeEnq = nullptr;
void shimFeEnqueue(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ctx.r5.u32 == 1 || n <= 8 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FE_ENQ] #%u mgr=%08X item=%08X type=%u a4=%08X a5=%08X lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
            (uint32_t)ctx.lr);
    s_origFeEnq(ctx, base);
}

// iter-350f: RTAM async request probes. WorldFlowXML init requests the FE
// flow FSM config through resMgr(DAT_829A2434) vt[8]=sub_826FAA70(sret,
// this, typeHash=0x6A1B8478, name, cbNode vt 0x82001674) -> scheduler item
// -> ResourceScheduler DAT_829A2424 (pump vt[7]=sub_826F44B8, waiter
// vt[8]=sub_826F4700 — COOPERATIVE, no pump thread). GC_REG #21 shows the
// translator resolve; GC_LOAD never fires for it = the item never gets
// serviced (or never enqueued). These name every async request + count
// pump activity + trace the flow completion cbs (0x82148498/82148768).
std::atomic<int>& s_inFlowInit = sr::kernel::g_srInFlowInit;
PPCFunc* s_origResReq = nullptr;
void shimResRequest(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-350f lesson (again): the ONE interesting call fell in the sample
    // gap. Unsampled for callback'd requests and anything inside the
    // WorldFlowXML init window; sampled for the bulk shader/texture preload.
    // iter-450V: the unsampled arm is now OPT-IN. It was written (iter-365e)
    // for a boot-preload window that issued a few hundred requests; the
    // front-end load issues ~30,000 inside the same flow-init window, and each
    // one costs a guest string read, a 220-char RES_REQ_X snprintf over 12
    // guest reads, and two lines through the global log spinlock - on the
    // guest's own thread. A drive run logged 30,368 RES_REQ_X in 96 s while
    // the game was trying to load. The bounded sample below still fires
    // always, so the request stream stays visible; only the storm is gone.
    static const bool s_resReqAll = [] {
        const char* e = std::getenv("SR_RES_REQ_TRACE");
        return e && e[0] == '1';
    }();
    const bool interesting = s_resReqAll &&
        (ctx.r7.u32 != 0 || s_inFlowInit.load(std::memory_order_relaxed) > 0);
    const bool logThis = interesting || n <= 600 || (n & (n - 1)) == 0;
    const uint32_t sret = ctx.r3.u32;
    char nm[64];
    if (logThis) {
        rdStr(base, ctx.r6.u32, nm, sizeof(nm));
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RES_REQ] #%u hash=%08X name='%s' cb=%08X lr=%08X", n,
            ctx.r5.u32, nm, ctx.r7.u32, (uint32_t)ctx.lr);
    }
    const uint32_t reqHash = ctx.r5.u32;
    s_origResReq(ctx, base);
    if (logThis && interesting) {
        const uint32_t rec = rd32(base, sret);
        char recw[220]; int o = 0;
        for (int k = 0; k < 12 && o < 200; ++k)
            o += snprintf(recw + o, sizeof(recw) - o, " %08X",
                          (rec >= 0x10000u) ? rd32(base, rec + k * 4) : 0);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RES_REQ_X] #%u name='%s' out={%08X %08X %08X %08X} rec[0..2C]=%s",
            n, nm, rec, rd32(base, sret + 4), rd32(base, sret + 8),
            rd32(base, sret + 12), recw);
        // Late re-dump for the world-flow request: state evolution (payload
        // +0xC, flags) 20s later names load-vs-stuck-vs-failed.
        //
        // ⛔ iter-450V: this comment said "one-shot" and the code was anything
        // but. It spawned ONE DETACHED OS THREAD PER MATCHING REQUEST, each
        // alive 80 s (4 x 20 s sleeps). Hash 0x6A1B8478 is the world-flow
        // request hash, and the front-end load issues ~30,000 of them inside
        // the flow-init window: one measured drive run spawned 29,912 threads,
        // ~29 GB of reserved stack, all resident at once.
        //
        // ⛔ CAUSALITY CORRECTION (iter-450W, director verdict on C-450Vc).
        // An earlier version of this comment asserted that the above was
        // "starving the input thread". THAT CLAIM IS WITHDRAWN. It was never
        // established. The evidence for it was a CROSS-ROLL A/B, and under
        // DOGMA non-determinism cross-roll comparisons cannot attribute
        // causality - in EITHER direction. What is actually supported: in the
        // two rolls measured, the guest's XamInputGetState rate fell to
        // ~0.05 Hz while presents continued, and gating this probe off produced
        // no measurable difference in any guest metric. Whether this probe
        // affected the guest at all remains UNDETERMINED.
        // The cap below is therefore justified as diagnostic hygiene - 30,000
        // threads for a "one-shot" probe is wrong on its own terms - and NOT as
        // a performance fix. Compare iter-365u (per-line fflush inside the log
        // spinlock), which was a measured within-run effect; this is not.
        //
        // Now: opt-in AND hard-capped. kFlowRecMax probes is all the state
        // evolution question ever needed - it asks what happens to A record
        // over time, not to thirty thousand of them.
        static const bool s_flowRecOn = [] {
            const char* e = std::getenv("SR_FLOWREC_PROBE");
            return e && e[0] == '1';
        }();
        static std::atomic<unsigned> s_flowRecSpawned{0};
        constexpr unsigned kFlowRecMax = 4;
        if (s_flowRecOn && reqHash == 0x6A1B8478u &&
            s_inFlowInit.load(std::memory_order_relaxed) > 0 &&
            s_flowRecSpawned.fetch_add(1, std::memory_order_relaxed) < kFlowRecMax) {
            std::thread([rec](uint8_t* gb) {
                for (int shot = 1; shot <= 4; ++shot) {
                    std::this_thread::sleep_for(std::chrono::seconds(20));
                    char w[220]; int p = 0;
                    for (int k = 0; k < 12 && p < 200; ++k)
                        p += snprintf(w + p, sizeof(w) - p, " %08X",
                                      (rec >= 0x10000u) ? rd32(gb, rec + k * 4) : 0);
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[FLOWREC] +%ds rec=%08X words=%s", shot * 20, rec, w);
                }
            }, base).detach();
        }
    }
}
PPCFunc* s_origSched7 = nullptr;
void shimSchedPump7(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0 ||
        s_inFlowInit.load(std::memory_order_relaxed) > 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RSCHED] pump7 #%u sched=%08X r4=%08X lr=%08X", n, ctx.r3.u32,
            ctx.r4.u32, (uint32_t)ctx.lr);
    s_origSched7(ctx, base);
}
PPCFunc* s_origSched8 = nullptr;
void shimSchedPump8(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0 ||
        s_inFlowInit.load(std::memory_order_relaxed) > 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RSCHED] pump8 #%u sched=%08X r4=%08X lr=%08X", n, ctx.r3.u32,
            ctx.r4.u32, (uint32_t)ctx.lr);
    s_origSched8(ctx, base);
}
// iter-350g: KV-store builder module init Function_8214D3F0 — builds the
// gameconfig key/value store (DAT_8296D87C) ONCE from the config text at
// cfg+0x14, gated on *(cfg+4)!=0. Bad rolls have the store EXIST but EMPTY
// (21/21 lookup misses) while gameconfig itself parsed a minute earlier.
// Log cfg flags + text-container words + resulting map counts.
PPCFunc* s_origKvBuild = nullptr;
void shimKvBuild(PPCContext& ctx, uint8_t* base)
{
    const uint32_t cfg = rd32(base, 0x8296D874u);
    char t14[40];
    rdStr(base, cfg ? rd32(base, cfg + 0x14u) : 0u, t14, sizeof(t14));
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[KV_BUILD] ENTER cfg=%08X +0=%08X +4=%08X +14=%08X +18=%08X +1C=%08X txt='%s' store=%08X lr=%08X",
        cfg, cfg ? rd32(base, cfg) : 0, cfg ? rd32(base, cfg + 4u) : 0,
        cfg ? rd32(base, cfg + 0x14u) : 0, cfg ? rd32(base, cfg + 0x18u) : 0,
        cfg ? rd32(base, cfg + 0x1Cu) : 0, t14, rd32(base, 0x8296D87Cu),
        (uint32_t)ctx.lr);
    s_origKvBuild(ctx, base);
    const uint32_t store = rd32(base, 0x8296D87Cu);
    const uint32_t map = (store >= 0x10000u) ? rd32(base, store + 8u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[KV_BUILD] EXIT store=%08X map=%08X m+4=%08X m+8=%08X m+C=%08X",
        store, map, (map >= 0x10000u) ? rd32(base, map + 4u) : 0,
        (map >= 0x10000u) ? rd32(base, map + 8u) : 0,
        (map >= 0x10000u) ? rd32(base, map + 0xCu) : 0);
}

// iter-350k: FE flow FSM START probes. The FSM parses + builds 43 states
// (4/4 post-350j) but never enters start_state. Candidates for the natural
// starter/ticker: sub_82360B00 (world-flow entity vt slot14 'start FSM',
// gated on entity+0x170 world obj + ACTR svc +0x84 + flag), and the actor-
// stack push-runners Function_82293108/822931C0 (push actor + run + pop).
PPCFunc* s_origFsmStart = nullptr;
void shimFsmStart(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0) {
        const uint32_t e = ctx.r3.u32;
        const uint32_t w170 = (e >= 0x10000u) ? rd32(base, e + 0x170) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FSM_START] B00 #%u ent=%08X +170=%08X +17c=%02X lr=%08X", n, e,
            w170, (e >= 0x10000u) ? base[e + 0x17c] : 0, (uint32_t)ctx.lr);
    }
    s_origFsmStart(ctx, base);
}
PPCFunc* s_origActRunA = nullptr;
void shimActorRunA(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FSM_START] run3108 #%u r3=%08X r4=%08X lr=%08X", n, ctx.r3.u32,
            ctx.r4.u32, (uint32_t)ctx.lr);
    // iter-357h: FULL state-array census (one-shot at #512): the boot chain
    // hangs off per-state CHILD content(+0x14) (nested FSMs / built-in command
    // states like PlayFMV & ShowScreen). Print {idx, script(+0x10),
    // content(+0x14)} for every state — the zero-content states name exactly
    // which cascade child-loads failed on our boot.
    if (n == 512) {
        const uint32_t f = ctx.r4.u32 >= 0x10000u ? ctx.r4.u32 : ctx.r3.u32;
        if (f >= 0x10000u) {
            const uint32_t cnt = rd32(base, f + 0x18);
            const uint32_t arr = rd32(base, f + 0x1C);
            if (arr >= 0x10000u && cnt && cnt <= 64) {
                for (uint32_t i = 0; i < cnt; ++i) {
                    const uint32_t st = rd32(base, arr + i * 4);
                    if (st < 0x10000u) continue;
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[FSM_CENSUS] idx=%u state=%08X vt=%08X script=%08X content=%08X",
                        i, st, rd32(base, st), rd32(base, st + 0x10),
                        rd32(base, st + 0x14));
                }
            }
        }
    }
    // iter-350k: one-shot FSM instance dump — args are (actorCtx r3,
    // fsmInst r4)? Body reads r4+8 cur / +0x18 count / +0x1C stateArr.
    // Dump BOTH interpretations (r3 and r4) at #2/#512 with the resolved
    // current-state object words (state name/script identify the graph).
    if (n == 2 || n == 512) {
        for (int which = 0; which < 2; ++which) {
            const uint32_t f = which ? ctx.r4.u32 : ctx.r3.u32;
            if (f < 0x10000u) continue;
            const uint32_t cur = rd32(base, f + 8);
            const uint32_t cnt = rd32(base, f + 0x18);
            const uint32_t arr = rd32(base, f + 0x1C);
            uint32_t st = 0;
            if (arr >= 0x10000u && cur < 0x100u && cur < cnt)
                st = rd32(base, arr + cur * 4);
            char w[200]; int o = 0;
            for (int k = 0; k < 10 && o < 180; ++k)
                o += snprintf(w + o, sizeof(w) - o, " %08X",
                              (st >= 0x10000u) ? rd32(base, st + k * 4) : 0);
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[FSM_DUMP] #%u %s=%08X cur=%08X cnt=%u arr=%08X state=%08X w=%s",
                n, which ? "r4" : "r3", f, cur, cnt, arr, st, w);
            // chase the state's pointer fields for names/scripts: each
            // candidate ptr -> print as string AND its first ptr -> string
            // (id strings / Lua script names identify the parked state).
            if (st >= 0x10000u) {
                for (int k = 3; k <= 4; ++k) {
                    const uint32_t p = rd32(base, st + k * 4);
                    if (p < 0x10000u) continue;
                    char s1[48], s2[48];
                    rdStr(base, p, s1, sizeof(s1));
                    const uint32_t pp = rd32(base, p);
                    rdStr(base, pp, s2, sizeof(s2));
                    char s3[48];
                    rdStr(base, rd32(base, p + 4), s3, sizeof(s3));
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[FSM_DUMP] #%u   state.w%d=%08X str='%s' [0]->'%s' [4]->'%s'",
                        n, k, p, s1, s2, s3);
                }
            }
        }
    }
    s_origActRunA(ctx, base);
}
// iter-350k: FSM state lifecycle walk trace. StateEnter sub_822918B0 /
// StateExit sub_822919D0 / StateShow Function_82291798 (mode 1/2/0 of
// sub_82259630). Logs state ptr + id-ish word +0x24 — the full walk
// sequence; the last ENTER without EXIT = the parked state.
#define SR_FSM_LIFE(NAME, VA, TAG)                                          \
    PPCFunc* s_orig##NAME = nullptr;                                        \
    void shim##NAME(PPCContext& ctx, uint8_t* base)                        \
    {                                                                       \
        static std::atomic<unsigned> s_n{0};                                \
        unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;       \
        const uint32_t st = ctx.r4.u32 >= 0x10000u ? ctx.r4.u32 : ctx.r3.u32; \
        const uint32_t act = (st >= 0x10000u) ? rd32(base, st + 0x18) : 0;  \
        if (n <= 200)                                                       \
            SR_LOG_BOOT(sr::LogSeverity::Warn,                              \
                "[FSM_LIFE] " TAG " #%u state=%08X id24=%08X w14=%08X act18=%08X actVt=%08X lr=%08X", \
                n, st, (st >= 0x10000u) ? rd32(base, st + 0x24) : 0,        \
                (st >= 0x10000u) ? rd32(base, st + 0x14) : 0, act,          \
                (act >= 0x10000u) ? rd32(base, act) : 0,                    \
                (uint32_t)ctx.lr);                                          \
        s_orig##NAME(ctx, base);                                            \
    }
SR_FSM_LIFE(StEnter, 0x822918B0, "ENTER")
SR_FSM_LIFE(StExit, 0x822919D0, "EXIT ")
SR_FSM_LIFE(StShow, 0x82291798, "SHOW ")
#undef SR_FSM_LIFE

// iter-350o: BOOT-SCREEN CTOR probe. Function_821D0D68 (class vt 0x82009798,
// factory table entry 2593). The factory calls it via an INDIRECT call through
// the table function-pointer, so a table patch catches every construction.
// Log every screen ctor: this=obj, and (post-ctor) its class vt + id (+0x58).
// Decisive: does the boot screen ever get CREATED at boot (activation gap) or
// NEVER (creation gap)?
PPCFunc* s_origBootScrCtor = nullptr;
void shimBootScrCtor(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t obj = ctx.r3.u32;
    s_origBootScrCtor(ctx, base);
    if (n <= 40)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[BOOTSCR_CTOR] #%u this=%08X classVt=%08X id+58=%08X lr=%08X", n,
            obj, (obj >= 0x10000u) ? rd32(base, obj) : 0,
            (obj >= 0x10000u) ? rd32(base, obj + 0x58) : 0, (uint32_t)ctx.lr);
}

// iter-350l: SPLASH ADVANCE probe. Function_822A8C68(psm): bootstraps
// ctr(psm+0x230) -1 -> 0 when renderer-ready byte (DAT_829A9384->+0x66,
// accessor sub_827759B8) == 0, then plays splash table entry
// (0x8291363C: EA_HD_logo...) via FUN_822b7f38. ctr stuck at -1 forever
// = either never called or byte nonzero. Log every call + the byte.
PPCFunc* s_origSplashAdv = nullptr;
void shimSplashAdvance(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t psm = ctx.r3.u32;
    const uint32_t rnd = rd32(base, 0x829A9384u);
    if (n <= 40 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SPLASH_ADV] #%u psm=%08X ctr=%08X renderer=%08X +66=%02X lr=%08X",
            n, psm, (psm >= 0x10000u) ? rd32(base, psm + 0x230) : 0, rnd,
            (rnd >= 0x10000u) ? base[rnd + 0x66] : 0xEE, (uint32_t)ctx.lr);
    s_origSplashAdv(ctx, base);
}

PPCFunc* s_origActRunB = nullptr;
void shimActorRunB(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FSM_START] run31C0 #%u r3=%08X r4=%08X lr=%08X", n, ctx.r3.u32,
            ctx.r4.u32, (uint32_t)ctx.lr);
    s_origActRunB(ctx, base);
}

// iter-350g: resMgr acquire vt[7]=sub_826FAAC0 — the branchy heart of the
// async load (record resolve -> scheduler item -> catalog open -> pump or
// spin-wait). ENTER/EXIT + args; unsampled inside the flow-init window.
PPCFunc* s_origResAcq = nullptr;
void shimResAcquire(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool inFlow = s_inFlowInit.load(std::memory_order_relaxed) > 0;
    const bool logThis = inFlow || n <= 16 || (n & (n - 1)) == 0;
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
    if (logThis)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RES_ACQ] #%u ENTER r3=%08X r4=%08X r5=%08X lr=%08X%s", n, a3, a4,
            a5, (uint32_t)ctx.lr, inFlow ? " <<FLOW" : "");
    s_origResAcq(ctx, base);
    if (logThis) {
        // r4 = record**: dump the record words on exit
        const uint32_t rec = rd32(base, a4);
        char w[120]; int o = 0;
        for (int k = 0; k < 8 && o < 100; ++k)
            o += snprintf(w + o, sizeof(w) - o, " %08X",
                          (rec >= 0x10000u) ? rd32(base, rec + k * 4) : 0);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RES_ACQ] #%u EXIT rec=%08X w=%s%s", n, rec, w, inFlow ? " <<FLOW" : "");
    }
}
// iter-350h: XML-config translate-from-buffer — translator vt[+0x1C] =
// sub_82190290(this, recHandle, cb, buf, size), called by the item run
// sub_82703EA0. rc=0 -> record marked FAILED silently (sub_826E5168).
// buf/size show whether the catalog open produced data at all.
PPCFunc* s_origXlTrans = nullptr;
void shimXlateTranslate(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool inFlow = s_inFlowInit.load(std::memory_order_relaxed) > 0;
    const bool logThis = inFlow || n <= 20 || (n & (n - 1)) == 0;
    const uint32_t buf = ctx.r6.u32, sz = ctx.r7.u32;
    char head[24];
    if (logThis) rdStr(base, buf, head, sizeof(head));
    if (logThis)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[XLTRANS] #%u ENTER this=%08X hnd=%08X cb=%08X buf=%08X sz=%u head='%s' lr=%08X%s",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, buf, sz, buf ? head : "",
            (uint32_t)ctx.lr, inFlow ? " <<FLOW" : "");
    s_origXlTrans(ctx, base);
    if (logThis)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[XLTRANS] #%u EXIT rc=%08X%s", n, ctx.r3.u32, inFlow ? " <<FLOW" : "");
}
// Translator "attach/dispose" vt[+0x20]=sub_82167DD0 (vt 0x82005F4C) — the
// terminal branch of sub_826FAAC0 when the record isn't in loading state:
// receives (this=translator, record, cb). If the flow request dies here
// silently, this names it.
PPCFunc* s_origXlAttach = nullptr;
void shimXlateAttach(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24 || (n & (n - 1)) == 0 ||
        s_inFlowInit.load(std::memory_order_relaxed) > 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[XLATE_ATTACH] #%u xl=%08X rec=%08X cb=%08X lr=%08X", n,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
    s_origXlAttach(ctx, base);
}
void shimKernelLoopRun(PPCContext& ctx, uint8_t* base)
{
    const uint32_t mgr = ctx.r3.u32;
    // iter-348: instant-re-entry storms (t≈116s, roll-dependent, ~300 Hz per
    // mgr) flooded 48K ENTER lines and hit the log cap at half-run, cutting
    // hunts short. Cap per-call logging; keep the storm visible as a sampled
    // count so it stays a first-class signal.
    // iter-349: ENTER counts dwarf EXIT counts — the "storm" may be RECURSION
    // (TickOnce's dispatch chain re-entering Run via a confused vtable slot).
    // Track per-thread depth; log mgr vt + period/accum (the tick-due inputs)
    // so both failure shapes (recursing vs never-due parked) name themselves.
    static thread_local unsigned t_depth = 0;
    ++t_depth;
    static std::atomic<unsigned> s_enterN{0};
    unsigned en = s_enterN.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-398g: runner-object registry for the [MOVGATE] per-runner dump.
    if (mgr) {
        for (int i = 0; i < 4; ++i) {
            uint32_t cur = g_kloopMgrs[i].load(std::memory_order_relaxed);
            if (cur == mgr) break;
            if (cur == 0 &&
                g_kloopMgrs[i].compare_exchange_strong(cur, mgr,
                                                       std::memory_order_relaxed))
                break;
        }
    }
    const bool logThis = en <= 24 || (en & (en - 1)) == 0 ||
                         (t_depth > 1 && t_depth <= 4);
    if (logThis) {
        const uint32_t vt = mgr ? rd32(base, mgr) : 0;
        const uint32_t perHi = mgr ? rd32(base, mgr + 0x48u) : 0;
        const uint32_t perLo = mgr ? rd32(base, mgr + 0x4Cu) : 0;
        const uint32_t accHi = mgr ? rd32(base, mgr + 0x68u) : 0;
        const uint32_t accLo = mgr ? rd32(base, mgr + 0x6Cu) : 0;
        // iter-349c: runtime vt SLOTS — Run's loop dispatches vt[6,9,8,1,7];
        // only vt[1] (tick thunk) receives mgr in r3, and the recursive Run
        // gets the same mgr — a runtime-patched vt[1]==Run would be exact.
        const uint32_t s1 = vt ? rd32(base, vt + 0x04) : 0;   // tick thunk
        const uint32_t s8 = vt ? rd32(base, vt + 0x20) : 0;   // gate
        const uint32_t s9 = vt ? rd32(base, vt + 0x24) : 0;   // TickOnce
        SR_LOG_BOOT(t_depth > 1 ? sr::LogSeverity::Error : sr::LogSeverity::Info,
            "[KLOOP] ENTER #%u depth=%u mgr=%08X vt=%08X{[1]=%08X [8]=%08X [9]=%08X} "
            "run=%02X pause=%02X period=%08X%08X accum=%08X%08X tid=%lu lr=%08X%s",
            en, t_depth, mgr, vt, s1, s8, s9,
            mgr ? (rd32(base, mgr + 0x40u) >> 8) & 0xFF : 0,
            mgr ? (rd32(base, mgr + 0x40u) >> 16) & 0xFF : 0,
            perHi, perLo, accHi, accLo, (unsigned long)GetCurrentThreadId(),
            (uint32_t)ctx.lr,
            t_depth > 1 ? "  <<RECURSIVE (TickOnce chain re-enters Run!)" : "");
    }
    if (mgr != 0 && mgr == rd32(base, 0x8296D8E0u))
        std::thread(simMgrGuardThread, base, mgr).detach();
    s_origKLoop(ctx, base);
    --t_depth;
    if (logThis)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[KLOOP] EXIT #%u mgr=%08X tid=%lu", en, mgr,
            (unsigned long)GetCurrentThreadId());
}

// iter-322: GameKernelMgr destroy watch. The SIM kernel loop gets
// StopAndJoin'd (Function_82720AC0: clears +0x40/+0x42, joins) ~110ms
// after start — via one of these vt-dispatched dtor/stop thunks (its only
// callers; all unreachable statically). Log every hit with callerLR to
// name the destroyer. SR_SIM_KEEP=1 additionally REFUSES the destroy when
// the target is the SIM mgr ([0x8296D8E0]) — A/B probe: does a surviving
// sim loop run the FE tick and climb the splash counter? (Band-aid probe
// only; natural fix = whatever recreate/transition is missing.)
PPCFunc* s_origKStop[9] = {};
constexpr uint32_t kKStopVAs[9] = {
    0x821660A8u, 0x821661E8u, 0x82166318u, 0x82166458u,
    0x821952A0u, 0x82195300u, 0x82195360u, 0x821953C0u, 0x82720CE0u };

template <int I>
void shimKStop(PPCContext& ctx, uint8_t* base)
{
    const uint32_t mgr = ctx.r3.u32;
    const uint32_t simMgr = rd32(base, 0x8296D8E0u);
    const bool isSim = mgr != 0 && mgr == simMgr;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[KSTOP] thunk=%08X mgr=%08X r4=%08X callerLR=%08X simMgr=%08X%s",
        kKStopVAs[I], mgr, ctx.r4.u32, (uint32_t)ctx.lr, simMgr,
        isSim ? (simKeepOn() ? " SIM -> KEEP (destroy refused)" : " SIM") : "");
    if (isSim && simKeepOn()) {
        ctx.r3.u64 = mgr;   // deleting-dtor callers expect r3 back
        return;
    }
    s_origKStop[I](ctx, base);
}

PPCFunc* s_origC8D0 = nullptr;   // FE state machine tick (sub_8235C8D0)
PPCFunc* s_origC780 = nullptr;   // FE SM splash-done reader (sub_8235C780)

// iter-318e: the FE transition XMsg now SENDS (XMsgStartIORequestEx 0x7001A
// fired for the first time ever) but the world never rebuilds — does the
// game-side FE state machine (the PSM+0x234 readers) ever tick at all?
void shimFeSmTick(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && (n <= 8 || (n % 2000) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FE_SM] tick #%u r3=%08X r4=%08X lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
    s_origC8D0(ctx, base);
}

void shimFeSmSplashRead(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && (n <= 8 || (n % 2000) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FE_SM] splashRead #%u r3=%08X lr=%08X",
            n, ctx.r3.u32, (uint32_t)ctx.lr);
    s_origC780(ctx, base);
}

PPCFunc* s_orig16E0 = nullptr;   // FmuiGlobalInit (sole eb80 writer)

// iter-316: eb80 (0x8296EB80 flow client) has exactly ONE writer — the tail
// of FmuiGlobalInit_821516E0: ec4c = sub_826D4930(...); eb80 = ec4c->vt[+0x10](0);
// then a PSM/FE registration block gated on [[cfg]+0x90]==1 (UserInterface=1,
// same offset as the movie gate). The fabricator sees eb80=0 forever, so one
// of: init never runs / ec4c null (DOGMA roll) / vt call returns 0 / dies
// before the tail. ENTER/EXIT dump names the stage.
void shimFmuiGlobalInit(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FMUI_GI] ENTER #%u obj=%08X ec4c=%08X eb80=%08X lr=%08X",
            n, ctx.r3.u32, rd32(base, 0x8296EC4Cu), rd32(base, 0x8296EB80u),
            (uint32_t)ctx.lr);
    s_orig16E0(ctx, base);
    if (flowWatchOn() && n <= 8) {
        const uint32_t cfg = rd32(base, 0x8296D874u);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FMUI_GI] EXIT  #%u ec4c=%08X eb80=%08X cfg=%08X uiGate[+90]=%08X",
            n, rd32(base, 0x8296EC4Cu), rd32(base, 0x8296EB80u),
            cfg, cfg ? rd32(base, cfg + 0x90u) : 0);
    }
}

void shimWorldFlowInit(PPCContext& ctx, uint8_t* base)
{
    if (flowWatchOn())
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FLOW] WorldFlowXML init ENTER obj=%08X lr=%08X", ctx.r3.u32, (uint32_t)ctx.lr);
    s_inFlowInit.fetch_add(1, std::memory_order_relaxed);
    s_origD508(ctx, base);
    s_inFlowInit.fetch_sub(1, std::memory_order_relaxed);
    if (flowWatchOn()) {
        // iter-350f: name the request — cfg+0x34 override else "WorldFlowXML";
        // flow mgr singleton DAT_8296DF74 published by Function_82225FB8.
        const uint32_t cfg = rd32(base, 0x8296D874u);
        char ov[40];
        rdStr(base, cfg ? cfg + 0x34u : 0u, ov, sizeof(ov));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FLOW] WorldFlowXML init EXIT rc=%08X cfgName='%s' flowMgr=%08X",
            ctx.r3.u32, ov, rd32(base, 0x8296DF74u));
    }
}

void shimMovieGate(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && (n <= 16 || (n % 2000) == 0)) {
        const uint32_t cfg = rd32(base, 0x8296D874u);
        char movie[64];
        rdStr(base, cfg ? cfg + 152u : 0u, movie, sizeof(movie));
        // cfg-object dump: golden 0x05B38A30 has {+0=1,+4=1, 'WorldData' text
        // at +0x14, 'WorldFlow=PCAWorldF' at +0x1E..} — parsed-store proof.
        char cfgTxt[40];
        rdStr(base, cfg ? cfg + 0x14u : 0u, cfgTxt, sizeof(cfgTxt));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FLOW] movieGate #%u cfg=%08X +0=%08X +4=%08X gate[+144]=%08X movie[+152]='%s' cfg+14txt='%s' lr=%08X",
            n, cfg, cfg ? rd32(base, cfg) : 0, cfg ? rd32(base, cfg + 4u) : 0,
            cfg ? rd32(base, cfg + 144u) : 0, movie, cfgTxt, (uint32_t)ctx.lr);
    }
    s_origFB78(ctx, base);
}

void shimSetInterfaceState(PPCContext& ctx, uint8_t* base)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (flowWatchOn() && n <= 24)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FLOW] SetInterfaceState #%u mod=%08X state=%u mode=%u lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
    s_orig45D0(ctx, base);
}

// iter-316c: RUNNER CENSUS. Golden-at-menu: render runner 0x8295C690 system
// (+8) = 0x82911370 = FSGameRenderMain obj (vt 0x82001820/38 family) whose
// vt[+4] roots builder sub_821490B8 lists 14 modules INCLUDING FMUI
// 0x8295FC8C (cell 0x82911460). Our natural walks (4/4) never dispatch
// FMUI's init => eb80 never publishes => no FE. Dump each runner {flags,
// system, sysVt, arrayPtr, arr[0]} + the 14 render root cells + FMUI module
// header words at 60/150/300s to name the divergence (empty array / wrong
// system / roots never built / triplet skip).
void dumpRunnerCensus(uint8_t* gb, int pass)
{
    static const uint32_t kRunners[4] = {0x8295C62Cu, 0x8295C65Cu, 0x8295C690u, 0x8295C6C0u};
    for (int i = 0; i < 4; ++i) {
        const uint32_t r = kRunners[i];
        const uint32_t sys = rd32(gb, r + 8);
        const uint32_t arr = rd32(gb, r + 0xC);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RUNNER] p%d %08X vt=%08X f=%08X sys=%08X sysVt=%08X arr=%08X arr0=%08X",
            pass, r, rd32(gb, r), rd32(gb, r + 4), sys, sys ? rd32(gb, sys) : 0,
            arr, arr ? rd32(gb, arr) : 0);
    }
    // render roots list lives at 0x8295C768 (built by sub_821490B8, once-flag
    // 0x8295C7A4); sim roots at 0x8295C7B8 (sub_82149300, flag 0x8295C830).
    char roots[420]; int o = 0;
    o += snprintf(roots + o, sizeof(roots) - o, "onceR=%08X onceS=%08X R:",
                  rd32(gb, 0x8295C7A4u), rd32(gb, 0x8295C830u));
    for (int i = 0; i < 15 && o < 380; ++i) {
        const uint32_t cell = rd32(gb, 0x8295C768u + i * 4);
        if (!cell) { o += snprintf(roots + o, sizeof(roots) - o, " end@%d", i); break; }
        o += snprintf(roots + o, sizeof(roots) - o, " %08X>%08X", cell, rd32(gb, cell));
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[RUNNER] p%d %s", pass, roots);
    char fm[200]; int f = 0;
    f += snprintf(fm + f, sizeof(fm) - f, "FMUI hdr:");
    for (int i = 0; i < 10; ++i)
        f += snprintf(fm + f, sizeof(fm) - f, " %08X", rd32(gb, 0x8295FC8Cu + i * 4));
    SR_LOG_BOOT(sr::LogSeverity::Info, "[RUNNER] p%d %s eb80=%08X ec4c=%08X",
                pass, fm, rd32(gb, 0x8296EB80u), rd32(gb, 0x8296EC4Cu));
}

// iter-316e BAND-AID (gated, retirable): FMSoundSystem's walk-init
// sub_82150A70 (module 0x8295F2F4) is the current terminal walk wedge
// ([SLOW_TICK]/[TICK_ENTRY] done=0 forever, 4/4 batch) — one of its ~20
// sound-TU sub-inits blocks in our stubbed-audio harness. Its ENTIRE body
// is gated on the gameconfig flag [[0x8296D874]+0x88]==1 (Sound flag):
// zero it and the init returns 1 immediately via the game's OWN config
// path. Audio is host-stubbed anyway. Retire when audio subsystem lands.
// SR_CFG_SOUND_OFF=0 disables. Ledger: docs/BANDAIDS.md.
void cfgSoundOffThread(uint8_t* gb)
{
    using namespace std::chrono_literals;
    static const bool on = [] {
        const char* e = std::getenv("SR_CFG_SOUND_OFF");
        return !e || e[0] != '0';
    }();
    if (!on) return;
    for (int i = 0; i < 2400; ++i) {   // up to 10 min, 250ms poll
        const uint32_t cfg = rd32(gb, 0x8296D874u);
        if (cfg) {
            const uint32_t soundFlag = rd32(gb, cfg + 0x88u);
            if (soundFlag == 1) {
                uint8_t* h = gb + cfg + 0x88u;
                h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 0;
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[CFG_SND] Sound flag [cfg+0x88] 1->0 (cfg=%08X) — FMSoundSystem init will no-op",
                    cfg);
                return;
            }
            if (soundFlag == 0 && i > 40) return;  // parsed as 0 already
        }
        std::this_thread::sleep_for(250ms);
    }
}

// iter-316d: NATURAL AUI DRIVER (wall-clock, independent thread).
// The natural boot now reaches: FMUI walk-init -> FmuiGlobalInit -> eb80 real
// flow client + UserInterface cfg gate ==1 (proven [FMUI_GI] EXIT). The next
// natural hop is ActivateUI(sub_822A2590) -> movieGate(sub_8214FB78) ->
// client->vt[+20](cfg+152) = frontend movie load — but ActivateUI is an
// ACTIONSCRIPT NATIVE (AS reg table 0x820C8328): on hardware the splash
// movie's script calls it. With IntroMovies not yet playable, nothing does.
// The old POST_SPLASH_FAB driver self-deadlocked two ways: (a) managerVA
// aliases the REAL client when eb80 was already published (its readiness
// check `eb80 != managerVA` is then false forever); (b) its 1s tick loop
// blocks inside the sub_822AB748 drive body (pump-stall hard-rule).
// This driver: own wall-clock thread, GuestCaller, fires ONCE when
// uiMgr + real eb80 (not the fabricated mgr, via g_fabMgrVA) are stable.
// SR_AUI_NATDRIVE=0 disables. Retirable when splash-movie AS runs naturally.
bool auiNatDriveOn()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_AUI_NATDRIVE");
        return !e || e[0] != '0';
    }();
    return v;
}

void auiNatDriveThread(uint8_t* gb)
{
    using namespace std::chrono_literals;
    int stable = 0;
    uint32_t eb80 = 0, uiMgr = 0;
    for (int i = 0; i < 1200; ++i) {   // up to 10 min
        std::this_thread::sleep_for(500ms);
        uiMgr = rd32(gb, 0x82911460u);
        eb80  = rd32(gb, 0x8296EB80u);
        const uint32_t fab = sr::kernel::g_fabMgrVA.load(std::memory_order_relaxed);
        // iter-318: heap-range sanity — b7_r0 fired on a SCRIBBLED eb80
        // (0x0032B940, FMUI never ran that roll). Real clients are heap objs.
        const bool eb80Sane = eb80 >= 0x84000000u && eb80 < 0x9F000000u;
        if (uiMgr && eb80 && eb80Sane && eb80 != fab) {
            if (++stable >= 3) break;
        } else {
            stable = 0;
        }
    }
    if (stable < 3) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[AUI_NAT] timeout — uiMgr=%08X eb80=%08X (never stable)", uiMgr, eb80);
        return;
    }
    const uint32_t cfg = rd32(gb, 0x8296D874u);
    const uint32_t gate144 = cfg ? rd32(gb, cfg + 144u) : 0;
    char movie[32];
    rdStr(gb, cfg ? cfg + 152u : 0u, movie, sizeof(movie));
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[AUI_NAT] preconditions met — uiMgr=%08X eb80=%08X cfg=%08X gate[+144]=%08X movie='%s'",
        uiMgr, eb80, cfg, gate144, movie);
    // iter-193c recipe: force gate only 0->1 (2 = later state, never rewind).
    if (cfg && gate144 == 0) {
        uint8_t* h = gb + cfg + 144u;
        h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 1;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[AUI_NAT] forced cfg gate[+144] 0->1");
    }
    // iter-318i: the iter-293 'MAIN'-uppercase recipe was WRONG for the
    // natural path and was THE ZERO-BLOB SOURCE: the native loader registers
    // the movie asset as lowercase "main" (BigFileAsset::Load census) and the
    // registry name-hash lookup is case-sensitive — uppercasing cfg+152 made
    // loadMovie('MAIN') miss the loaded "main" wrapper, yielding the empty
    // record whose junk descriptor the AS-VM then executed (the .data sweep).
    // Retail gameconfig is MainScreen=main; leave it exactly as parsed.
    // SR_AUI_UPPER=1 restores the old rewrite for A/B.
    if (cfg) {
        static const bool s_upper = [] {
            const char* e = std::getenv("SR_AUI_UPPER");
            return e && e[0] == '1';
        }();
        uint8_t* nameHost = gb + cfg + 152u;
        if (s_upper && (nameHost[0] == 'm' && nameHost[1] == 'a' &&
                        nameHost[2] == 'i' && nameHost[3] == 'n')) {
            std::memcpy(nameHost, "MAIN", 5);
            SR_LOG_BOOT(sr::LogSeverity::Info, "[AUI_NAT] cfg+152 -> \"MAIN\" (A/B)");
        } else {
            char nm[16];
            rdStr(gb, cfg + 152u, nm, sizeof(nm));
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AUI_NAT] cfg+152 movie name LEFT NATURAL: '%s'", nm);
        }
    }
    sr::kernel::GuestCaller gc("AUI_NAT");
    if (!gc.ok()) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUI_NAT] GuestCaller setup failed");
        return;
    }
    // Call through the dispatch table so shimActivateUI traces it.
    PPCFunc* fn = PPC_LOOKUP_FUNC(gb, 0x822A2590u);
    if (!fn) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUI_NAT] no dispatch entry for 822A2590");
        return;
    }
    auto driveOnce = [&](int shot) {
        sr::kernel::GuestArgs a{};
        a.r3 = 0;
        uint32_t exc = 0;
        const bool ret = gc.call(fn, a, &exc);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[AUI_NAT] ActivateUI shot#%d %s rc=%08X exc=%08X — aptBase=%08X aptCount=%08X eb80=%08X resMgr=%08X",
            shot, ret ? "returned" : "FAULTED", ret ? gc.ctx().r3.u32 : 0, exc,
            rd32(gb, 0x829A19B8u), rd32(gb, 0x829A19B0u), rd32(gb, 0x8296EB80u),
            rd32(gb, 0x829A23E0u));
    };
    driveOnce(1);
    // iter-318l: SHOT #2 PRE-TEARDOWN. Shot #1's own loadMovie races the
    // package walk it triggers (the iter-198p class: the too-early request's
    // dead node occupies the level slot). b5_r0 PROVED a second ActivateUI
    // after registration INSTALLS the movie (aptCount=1 — first in project
    // history) — but it fired post-teardown into a dirty world. Fire it
    // 15s after shot #1 while the world is still clean: packages registered,
    // managers alive → the natural resolve connects.
    std::this_thread::sleep_for(std::chrono::seconds(15));
    if (rd32(gb, 0x8296EB80u) != 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[AUI_NAT] pre-teardown second shot (packages registered, world clean)");
        driveOnce(2);
    }
    // iter-317f: shot #1 lands mid-SPLASH phase — the PSM splash-done
    // transition (~90-100s: sub_822A8C68 counter → sub_822AB748 dispatch(-1)
    // → worker ExTerminateThread) DESTROYS the boot-phase resMgr (829A23E0
    // reset to static default 0x82088310) and with it everything shot #1
    // built. On hardware the FE phase then rebuilds and the splash movie's
    // AS calls ActivateUI natively. Bridge v2: watch for the teardown
    // (resMgr leaves heap range), wait for a NEW heap manager (the FE-phase
    // rebuild) or 45s, then drive again. Up to 3 post-teardown shots.
    const uint32_t bootMgr = rd32(gb, 0x829A23E0u);
    bool tornDown = false;
    int shots = 2;
    for (int i = 0; i < 600 && shots < 4; ++i) {   // up to 5 min
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const uint32_t mgr = rd32(gb, 0x829A23E0u);
        const bool heapMgr = mgr >= 0x84000000u && mgr < 0x9F000000u;
        if (!tornDown) {
            if (mgr != bootMgr && !heapMgr) {
                tornDown = true;
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[AUI_NAT] boot-phase teardown detected (resMgr %08X -> %08X) — re-arming",
                    bootMgr, mgr);
            }
            continue;
        }
        // torn down: fire when a fresh heap manager appears, or every ~45s.
        static int sinceTeardown = 0;
        ++sinceTeardown;
        if (heapMgr || (sinceTeardown % 90) == 0) {
            ++shots;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AUI_NAT] post-teardown drive (mgr=%08X heap=%d)", mgr, heapMgr ? 1 : 0);
            driveOnce(shots);
            if (heapMgr) break;   // FE-phase manager exists; one shot against it
        }
    }
}

// iter-318f: THE FE-SM KICK — the one-write bridge. FeMasterTick calls the
// FE state machine sub_8235C8D0(dt, smPtr=[0x829761E0]) unconditionally
// every frame; the SM is a switch on [smObj+4] and our boot sits in state 0
// (no-op forever). State 4 = the FULL FE advance (sub_8235C780: screen
// build FUN_822a3030, screen dispatch, menu input keys 0x41/0x49) then
// settles to 0 (golden's at-menu value). On hardware the XMsg 0x7001A
// receiver's job is exactly this write. Kick once splash-done + eb80 real;
// re-kick after a teardown. SR_FE_SM_KICK=0 disables.
void feSmKickThread(uint8_t* gb)
{
    using namespace std::chrono_literals;
    static const bool on = [] {
        const char* e = std::getenv("SR_FE_SM_KICK");
        return !e || e[0] != '0';
    }();
    if (!on) return;
    // iter-319c: the FE state machine belongs to FMfmvManager (module obj
    // 0x8295F1C4, walk-init Function_8214D780 -> lazy ctor Function_8235F1E0
    // -> publishes [0x829761E0]) — a walker-invisible module (FMGameEntity
    // class: in no walked roots). In the poison-free boot nothing constructs
    // the SM, so the kick had no target. Drive the game's own idempotent
    // lazy ctor once when the world is up, then kick as before.
    {
        for (int w = 0; w < 360; ++w) {   // wait for aptMgr (world up), 3 min
            if (rd32(gb, 0x829A23E0u) >= 0x84000000u) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        sr::kernel::GuestCaller gc("FE_PHASE_CTORS");
        // iter-320b: with the E630 timeout retired the natural sim walk reaches
        // FMfmvManager (root #8) and FMGameEntity (root #10) itself — verdict
        // batch showed lastInit past them at #12 (FMAnim) while THIS thread's
        // concurrent ctor drive sat mid-guest-call (lock-inversion risk with
        // the walk). Drives are legacy-mode only now; natural mode trusts the
        // walk. The state-4 kick below stays in all modes (write-only, guarded).
        if (!sr::kernel::naturalModuleInitOn() && gc.ok()) {
            if (rd32(gb, 0x829761E0u) == 0) {
                if (PPCFunc* ctor = PPC_LOOKUP_FUNC(gb, 0x8235F1E0u)) {
                    sr::kernel::GuestArgs a{};
                    uint32_t exc = 0;
                    gc.call(ctor, a, &exc);
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[FE_SM_KICK] FMfmvManager SM lazy-ctor driven — [829761E0]=%08X exc=%08X",
                        rd32(gb, 0x829761E0u), exc);
                }
            }
        }
    }
    int kicks = 0;
    bool armed = true;
    int fmEntityTries = 0;
    for (int i = 0; i < 2400 && kicks < 4; ++i) {   // up to 20 min
        std::this_thread::sleep_for(500ms);
        // iter-321: GameKernel mgr census — the loops park at idle
        // (lr=82720C30) after full natural init; name the gate (pause byte
        // +0x41, run +0x42, period +0x48, clock accum +0x60/+0x68/+0x70).
        if (i % 20 == 0 && i < 600) {
            for (int m = 0; m < 4; ++m) {
                const uint32_t mgr = rd32(gb, 0x8296D8D8u + m * 4u);
                if (mgr < 0x10000u || mgr >= 0x9F000000u) continue;
                const uint32_t s40 = rd32(gb, mgr + 0x40u);
                const uint32_t per_hi = rd32(gb, mgr + 0x48u), per_lo = rd32(gb, mgr + 0x4Cu);
                const uint32_t acc_hi = rd32(gb, mgr + 0x68u), acc_lo = rd32(gb, mgr + 0x6Cu);
                const uint32_t last_hi = rd32(gb, mgr + 0x60u), last_lo = rd32(gb, mgr + 0x64u);
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[KMGR] t=%ds mgr%d=%08X s40=%08X per=%08X%08X last=%08X%08X acc=%08X%08X clkSingleton=%08X",
                    i / 2, m, mgr, s40, per_hi, per_lo, last_hi, last_lo,
                    acc_hi, acc_lo, rd32(gb, 0x829A2BF0u));
            }
        }
        // iter-319f: the FMGameEntity SCREEN-SYSTEM drive in splash_hooks
        // sits under a gated FE-tick branch that never executes; run it
        // HERE (the thread that provably runs), retrying until the config
        // cells exist. Walker-invisible module (iter-297h): registry DD0C +
        // managers built by sub_8214FF48 with a scratch module obj — the FE
        // advance's last empty input.
        if (!sr::kernel::naturalModuleInitOn() && fmEntityTries < 3 &&
            rd32(gb, 0x8296DD0Cu) == 0 &&
            rd32(gb, 0x8296D868u) != 0 && rd32(gb, 0x8296D8B8u) != 0) {
            ++fmEntityTries;
            sr::kernel::GuestCaller gcE("SCREEN_SYS_CTOR");
            if (gcE.ok()) {
                if (const uint32_t scratch = sr::kernel::guestHeapAlloc(0x100)) {
                    if (PPCFunc* fe = PPC_LOOKUP_FUNC(gb, 0x8214FF48u)) {
                        sr::kernel::GuestArgs a{};
                        a.r3 = scratch;
                        uint32_t exc = 0;
                        gcE.call(fe, a, &exc);
                        SR_LOG_BOOT(sr::LogSeverity::Info,
                            "[SCREEN_SYS] FMGameEntity init driven (try %d) — DD0C=%08X DD08=%08X DD30=%08X exc=%08X",
                            fmEntityTries, rd32(gb, 0x8296DD0Cu),
                            rd32(gb, 0x8296DD08u), rd32(gb, 0x8296DD30u), exc);
                    }
                }
            }
        }
        const uint32_t psm = rd32(gb, 0x8296EDACu);
        const uint32_t done = psm ? (rd32(gb, psm + 0x234u) >> 24) : 0;
        // iter-357g: golden-at-menu has counter(+0x230)=-1 FOREVER and done(+0x234)=1
        // (set <30ms after PSM creation; the splash counter ladder NEVER runs on the
        // healthy boot). Ours = the open question — log the pair once, then on change.
        {
            static uint32_t s_lastPair = 0xEEEEEEEE;
            const uint32_t counter = psm ? rd32(gb, psm + 0x230u) : 0xDEADDEAD;
            const uint32_t pair = (counter << 8) | done;
            if (psm && pair != s_lastPair) {
                s_lastPair = pair;
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PSM_FLAGS] psm=%08X counter(+0x230)=%08X done(+0x234 byte)=%u",
                    psm, counter, done);
            }
        }
        const uint32_t smPtr = rd32(gb, 0x829761E0u);
        const bool smSane = smPtr >= 0x84000000u && smPtr < 0x9F000000u;
        const uint32_t state = smSane ? rd32(gb, smPtr + 4u) : 0xDEAD;
        if (!armed) {
            // re-arm after a teardown (sm ptr cycles) or if state returned 0
            if (!smSane || state == 0) armed = true;
            continue;
        }
        // iter-338: THE APT LOGIC DRIVE TEST. Re-publish the latched MAIN Apt
        // context to 0x829A17BC and drive the logic tick sub_826C0888 — the
        // executor runs against a VALID clip manager (ctx+0x20) instead of the
        // garbage it currently gets. Decisive: does DEF_EXEC fire?
        {
            static const bool s_logicDrive = [] {
                const char* e = std::getenv("SR_APT_LOGIC_DRIVE");
                return !e || e[0] != '0';
            }();
            const uint32_t menuCtx = sr::kernel::g_menuCtx.load(std::memory_order_relaxed);
            static int s_ld = 0;
            if (s_logicDrive && s_ld < 60 &&
                menuCtx >= 0x82000000u && menuCtx < 0x8F000000u) {
                // clip mgr must be sane before we drive (ctx+0x20)
                const uint32_t clipMgr = rd32(gb, menuCtx + 0x20u);
                if (clipMgr >= 0x10000u && clipMgr < 0xC0000000u) {
                    ++s_ld;
                    // re-publish the ctx (BE) so movieGate + logic tick see it
                    uint8_t* h = gb + 0x829A17BCu;
                    h[0] = (menuCtx >> 24) & 0xFF; h[1] = (menuCtx >> 16) & 0xFF;
                    h[2] = (menuCtx >> 8) & 0xFF;  h[3] = menuCtx & 0xFF;
                    sr::kernel::GuestCaller gcL("APT_LOGIC_TICK");
                    if (gcL.ok()) {
                        // iter-339: pending array empty (iter-338) = MAIN never
                        // queued into this ctx. Drive movieGate sub_8214FB78
                        // (loads the config MAIN movie into the PUBLISHED ctx)
                        // ONCE, then advance the logic tick so the loaded movie
                        // builds + frame-advances (AS runs). uiCfg = the AUI
                        // client config (0x8295FC8C @ 0x82911460).
                        const uint32_t uiCfg = rd32(gb, 0x82911460u);
                        static bool s_movieGated = false;
                        if (!s_movieGated && uiCfg >= 0x10000u && uiCfg < 0xC0000000u) {
                            s_movieGated = true;
                            if (PPCFunc* mg = PPC_LOOKUP_FUNC(gb, 0x8214FB78u)) {
                                sr::kernel::GuestArgs a{};
                                a.r3 = uiCfg;
                                uint32_t exc = 0;
                                gcL.call(mg, a, &exc);
                                SR_LOG_BOOT(sr::LogSeverity::Info,
                                    "[APT_LOGIC_DRIVE] movieGate(uiCfg=%08X) driven — name54=%08X state48=%08X pend=%08X exc=%08X",
                                    uiCfg, rd32(gb, uiCfg + 0x54u), rd32(gb, uiCfg + 0x48u),
                                    rd32(gb, clipMgr + 4u), exc);
                            }
                        }
                        if (PPCFunc* tick = PPC_LOOKUP_FUNC(gb, 0x826C0888u)) {
                            sr::kernel::GuestArgs a{};
                            uint32_t exc = 0;
                            gcL.call(tick, a, &exc);
                            if (s_ld <= 6 || (s_ld % 16) == 0)
                                SR_LOG_BOOT(sr::LogSeverity::Info,
                                    "[APT_LOGIC_DRIVE] #%d ctx=%08X clipMgr=%08X pend=%08X exc=%08X",
                                    s_ld, menuCtx, clipMgr, rd32(gb, clipMgr + 4u), exc);
                        }
                    }
                }
            }
        }
        // iter-330 THE CORRECTED 0x7001A RECEIVER: consume the queued
        // interface-state request and perform it with the game's own
        // SetInterfaceState_sub_821745D0(auiMod, state, 0) — the iter-297g
        // PROVEN contract (state 4 = golden menu idle; holds once set).
        if (sr::kernel::g_xmsg7001A_pending.load(std::memory_order_acquire)) {
            const uint32_t auiMod = rd32(gb, 0x8296D86Cu);
            const uint32_t payload = sr::kernel::g_xmsg7001A_payload.load();
            uint32_t state = payload & 0xFFFFu;
            // iter-331b: state 0 = the intro-movie area; our FMV decode never
            // raises movie-end so intros LOOP and the {2,4} menu transition
            // never sends. Deliver state 4 (menu idle) directly — the exact
            // behavior the game's own IntroMovies=0 config produces.
            // SR_SKIP_INTRO=0 restores intro playback (for when FMV
            // completion lands).
            static const bool s_skipIntro = [] {
                const char* e = std::getenv("SR_SKIP_INTRO");
                return !e || e[0] != '0';
            }();
            if (s_skipIntro && state == 0) {
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[IFACE_RX] intro area requested — delivering state 4 (IntroMovies=0 semantics)");
                state = 4;
            }
            if (auiMod >= 0x10000u && auiMod < 0xC0000000u) {
                sr::kernel::g_xmsg7001A_pending.store(false, std::memory_order_release);
                sr::kernel::GuestCaller gcS("IFACE_STATE_RX");
                if (gcS.ok()) {
                    if (PPCFunc* fn = PPC_LOOKUP_FUNC(gb, 0x821745D0u)) {
                        sr::kernel::GuestArgs a{};
                        a.r3 = auiMod;
                        a.r4 = state;
                        a.r5 = 0;
                        uint32_t exc = 0;
                        gcS.call(fn, a, &exc);
                        static int s_rx = 0;
                        ++s_rx;
                        if (s_rx <= 8 || (s_rx & (s_rx - 1)) == 0)
                            SR_LOG_BOOT(sr::LogSeverity::Info,
                                "[IFACE_RX] #%d SetInterfaceState(mod=%08X, state=%u, 0) exc=%08X — selector=%08X state1320=%08X",
                                s_rx, auiMod, state, exc,
                                rd32(gb, auiMod + 0x1310u), rd32(gb, auiMod + 0x1320u));
                        // iter-333: the transition has TWO halves. Interface
                        // state (above) selects the render client — the red
                        // base layer proved it draws. The SCREEN BUILD is the
                        // FE-SM state-4 case sub_8235C8D0(SM)→sub_8235C780→
                        // FUN_822a3030(PSM, screenId) + screen dispatch. The
                        // FE tick calls sub_8235C8D0 on the SIM thread, but
                        // that thread survives without ticking its FE update
                        // (Frida-proven: write state 4 into the SM, tick never
                        // consumed it). All screen-state globals ARE populated
                        // (0x8296E8C8/EDB0/DD0C live). So drive the FE-SM tick
                        // DIRECTLY here with the current SM after arming state
                        // 4 — this thread provably runs. SR_FE_SCREEN_BUILD=0
                        // disables.
                        static const bool s_screenBuild = [] {
                            const char* e = std::getenv("SR_FE_SCREEN_BUILD");
                            return !e || e[0] != '0';
                        }();
                        const uint32_t sm = rd32(gb, 0x829761E0u);
                        if (s_screenBuild && state == 4 &&
                            sm >= 0x84000000u && sm < 0x9F000000u) {
                            uint8_t* h = gb + sm + 4u;
                            h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 4;  // SM state = 4
                            if (PPCFunc* tick = PPC_LOOKUP_FUNC(gb, 0x8235C8D0u)) {
                                sr::kernel::GuestArgs a2{};
                                a2.r3 = sm;
                                uint32_t exc2 = 0;
                                gcS.call(tick, a2, &exc2);
                                static int s_sb = 0;
                                ++s_sb;
                                if (s_sb <= 8 || (s_sb & (s_sb - 1)) == 0)
                                    SR_LOG_BOOT(sr::LogSeverity::Info,
                                        "[FE_SCREEN] #%d sub_8235C8D0(sm=%08X, state4) driven — screenId=%08X exc=%08X",
                                        s_sb, sm,
                                        rd32(gb, rd32(gb, 0x8296E8C8u) + 0xCu), exc2);
                            }
                        }
                    }
                }
            }
        }
        // iter-329: the SM is built by the sim walk (fmvManager init →
        // Function_8235F1E0, unconditional) but the CELL still gets wiped on
        // some rolls (residual base==0 grant overlap; scribble family's last
        // breath). The ctor is idempotent — re-run the game's own builder
        // when the cell reads 0 well after walks. Retire with the overlap
        // root fix. SR_SM_REBUILD=0 disables.
        static const bool s_smRebuild = [] {
            const char* e = std::getenv("SR_SM_REBUILD");
            return !e || e[0] != '0';
        }();
        static int s_smRebuilds = 0;
        if (s_smRebuild && i > 40 && s_smRebuilds < 5 &&
            rd32(gb, 0x829761E0u) == 0) {
            sr::kernel::GuestCaller gcR("SM_REBUILD");
            if (gcR.ok()) {
                if (PPCFunc* ctor = PPC_LOOKUP_FUNC(gb, 0x8235F1E0u)) {
                    sr::kernel::GuestArgs a{};
                    uint32_t exc = 0;
                    gcR.call(ctor, a, &exc);
                    ++s_smRebuilds;
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[SM_REBUILD] #%d Function_8235F1E0 re-driven — [829761E0]=%08X exc=%08X",
                        s_smRebuilds, rd32(gb, 0x829761E0u), exc);
                }
            }
        }
        // iter-325: the splash now cycles to 5 and fires 0x7001A repeatedly
        // (iter-324b) but no kick ever logged — show which condition fails.
        if (i % 40 == 0 && i < 1200) {
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[KICK_DIAG] t=%ds smPtr=%08X state=%08X psm=%08X done=%u armed=%d",
                i / 2, smPtr, state, psm, done, (int)armed);
        }
        if (done && smSane && state == 0) {
            uint8_t* h = gb + smPtr + 4u;
            h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 4;
            ++kicks;
            armed = false;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[FE_SM_KICK] #%d wrote state 4 -> smObj=%08X (psm=%08X done=1) — FE advance next tick",
                kicks, smPtr, psm);
            std::this_thread::sleep_for(3s);
        }
    }
}

// Installer thread: waits for guest memory AND the dispatch table, patches once.
// iter-359c: the table check is load-bearing — the GPU trace replay registers
// its arena as global guest memory WITHOUT booting the guest, so g_dispatchTable
// stays null and the old memory-only wait dereferenced it (fatal AV under the
// VEH; killed every replay run since iter-358k).
struct FlowWatchStarter {
    FlowWatchStarter()
    {
        std::thread([] {
            uint8_t* gb = nullptr;
            size_t sz = 0;
            for (;;) {
                sr::kernel::getGlobalGuestMemory(&gb, &sz);
                if (gb && sr::kernel::g_dispatchTable) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            s_origD508 = PPC_LOOKUP_FUNC(gb, 0x8214D508u);
            s_origFB78 = PPC_LOOKUP_FUNC(gb, 0x8214FB78u);
            s_orig45D0 = PPC_LOOKUP_FUNC(gb, 0x821745D0u);
            s_orig23A0 = PPC_LOOKUP_FUNC(gb, 0x826D23A0u);
            s_orig2690 = PPC_LOOKUP_FUNC(gb, 0x826D2690u);
            if (s_orig2690) PPC_LOOKUP_FUNC(gb, 0x826D2690u) = shimAptLoadByPath;
            s_origB748 = PPC_LOOKUP_FUNC(gb, 0x822AB748u);
            s_orig5FE8 = PPC_LOOKUP_FUNC(gb, 0x826D5FE8u);
            s_orig91F0 = PPC_LOOKUP_FUNC(gb, 0x826D91F0u);
            if (s_orig5FE8) PPC_LOOKUP_FUNC(gb, 0x826D5FE8u) = shimOnPackageBound;
            if (s_orig91F0) PPC_LOOKUP_FUNC(gb, 0x826D91F0u) = shimCreatePackage;
            s_orig6B80 = PPC_LOOKUP_FUNC(gb, 0x826E6B80u);
            if (s_orig6B80) PPC_LOOKUP_FUNC(gb, 0x826E6B80u) = shimNameLookup;
            s_origKLoop = PPC_LOOKUP_FUNC(gb, 0x82720B50u);
            if (s_origKLoop) PPC_LOOKUP_FUNC(gb, 0x82720B50u) = shimKernelLoopRun;
            s_origKStop[0] = PPC_LOOKUP_FUNC(gb, kKStopVAs[0]);
            if (s_origKStop[0]) PPC_LOOKUP_FUNC(gb, kKStopVAs[0]) = shimKStop<0>;
            s_origKStop[1] = PPC_LOOKUP_FUNC(gb, kKStopVAs[1]);
            if (s_origKStop[1]) PPC_LOOKUP_FUNC(gb, kKStopVAs[1]) = shimKStop<1>;
            s_origKStop[2] = PPC_LOOKUP_FUNC(gb, kKStopVAs[2]);
            if (s_origKStop[2]) PPC_LOOKUP_FUNC(gb, kKStopVAs[2]) = shimKStop<2>;
            s_origKStop[3] = PPC_LOOKUP_FUNC(gb, kKStopVAs[3]);
            if (s_origKStop[3]) PPC_LOOKUP_FUNC(gb, kKStopVAs[3]) = shimKStop<3>;
            s_origKStop[4] = PPC_LOOKUP_FUNC(gb, kKStopVAs[4]);
            if (s_origKStop[4]) PPC_LOOKUP_FUNC(gb, kKStopVAs[4]) = shimKStop<4>;
            s_origKStop[5] = PPC_LOOKUP_FUNC(gb, kKStopVAs[5]);
            if (s_origKStop[5]) PPC_LOOKUP_FUNC(gb, kKStopVAs[5]) = shimKStop<5>;
            s_origKStop[6] = PPC_LOOKUP_FUNC(gb, kKStopVAs[6]);
            if (s_origKStop[6]) PPC_LOOKUP_FUNC(gb, kKStopVAs[6]) = shimKStop<6>;
            s_origKStop[7] = PPC_LOOKUP_FUNC(gb, kKStopVAs[7]);
            if (s_origKStop[7]) PPC_LOOKUP_FUNC(gb, kKStopVAs[7]) = shimKStop<7>;
            s_origKStop[8] = PPC_LOOKUP_FUNC(gb, kKStopVAs[8]);
            if (s_origKStop[8]) PPC_LOOKUP_FUNC(gb, kKStopVAs[8]) = shimKStop<8>;
            s_origPsmInvoke = PPC_LOOKUP_FUNC(gb, 0x822B7370u);
            if (s_origPsmInvoke) PPC_LOOKUP_FUNC(gb, 0x822B7370u) = shimPsmStateInvoke;
            s_origSweepExec = PPC_LOOKUP_FUNC(gb, 0x82776850u);
            if (s_origSweepExec) PPC_LOOKUP_FUNC(gb, 0x82776850u) = shimSweepExec;
            s_origRdy2 = PPC_LOOKUP_FUNC(gb, 0x82776EB0u);
            if (s_origRdy2) PPC_LOOKUP_FUNC(gb, 0x82776EB0u) = shimRdy2;
            s_origTechExec = PPC_LOOKUP_FUNC(gb, 0x8277B998u);
            if (s_origTechExec) PPC_LOOKUP_FUNC(gb, 0x8277B998u) = shimTechExec;
            s_origRenderEnable = PPC_LOOKUP_FUNC(gb, 0x82775868u);
            if (s_origRenderEnable) PPC_LOOKUP_FUNC(gb, 0x82775868u) = shimRenderEnable;
            s_origRSysTick = PPC_LOOKUP_FUNC(gb, 0x821491A0u);
            if (s_origRSysTick) PPC_LOOKUP_FUNC(gb, 0x821491A0u) = shimRSysTick;
            s_origEnvUpd = PPC_LOOKUP_FUNC(gb, 0x8230C038u);
            if (s_origEnvUpd) PPC_LOOKUP_FUNC(gb, 0x8230C038u) = shimEnvUpd;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHIMS] sweep=%d rdy=%d tech=%d", s_origSweepExec != nullptr,
                s_origRdy2 != nullptr, s_origTechExec != nullptr);
            s_origFeTick = PPC_LOOKUP_FUNC(gb, 0x821494C0u);
            if (s_origFeTick) PPC_LOOKUP_FUNC(gb, 0x821494C0u) = shimFeMasterTick;
            s_origRdyGate = PPC_LOOKUP_FUNC(gb, 0x82776EB0u);
            if (s_origRdyGate) PPC_LOOKUP_FUNC(gb, 0x82776EB0u) = shimRenderReadyGate;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[RDY_INSTALL] sub_82776EB0 table entry=%p (null => not table-dispatched) installed=%d",
                (void*)s_origRdyGate, s_origRdyGate ? 1 : 0);
            s_origC8D0 = PPC_LOOKUP_FUNC(gb, 0x8235C8D0u);
            if (s_origC8D0) PPC_LOOKUP_FUNC(gb, 0x8235C8D0u) = shimFeSmTick;
            s_origC780 = PPC_LOOKUP_FUNC(gb, 0x8235C780u);
            if (s_origC780) PPC_LOOKUP_FUNC(gb, 0x8235C780u) = shimFeSmSplashRead;
            s_orig16E0 = PPC_LOOKUP_FUNC(gb, 0x821516E0u);
            if (s_orig16E0) PPC_LOOKUP_FUNC(gb, 0x821516E0u) = shimFmuiGlobalInit;
            s_orig2590 = PPC_LOOKUP_FUNC(gb, 0x822A2590u);
            if (s_origB748) PPC_LOOKUP_FUNC(gb, 0x822AB748u) = shimPsmDispatch;
            if (s_orig2590) PPC_LOOKUP_FUNC(gb, 0x822A2590u) = shimActivateUI;
            if (s_origD508) PPC_LOOKUP_FUNC(gb, 0x8214D508u) = shimWorldFlowInit;
            if (s_origFB78) PPC_LOOKUP_FUNC(gb, 0x8214FB78u) = shimMovieGate;
            if (s_orig45D0) PPC_LOOKUP_FUNC(gb, 0x821745D0u) = shimSetInterfaceState;
            if (s_orig23A0) PPC_LOOKUP_FUNC(gb, 0x826D23A0u) = shimAptDrain;
            // iter-350f RTAM async request/pump probes (vtable-dispatched)
            s_origResReq = PPC_LOOKUP_FUNC(gb, 0x826FAA70u);
            if (s_origResReq) PPC_LOOKUP_FUNC(gb, 0x826FAA70u) = shimResRequest;
            s_origSched7 = PPC_LOOKUP_FUNC(gb, 0x826F44B8u);
            if (s_origSched7) PPC_LOOKUP_FUNC(gb, 0x826F44B8u) = shimSchedPump7;
            s_origSched8 = PPC_LOOKUP_FUNC(gb, 0x826F4700u);
            if (s_origSched8) PPC_LOOKUP_FUNC(gb, 0x826F4700u) = shimSchedPump8;
            s_origXlAttach = PPC_LOOKUP_FUNC(gb, 0x82167DD0u);
            if (s_origXlAttach) PPC_LOOKUP_FUNC(gb, 0x82167DD0u) = shimXlateAttach;
            s_origXlTrans = PPC_LOOKUP_FUNC(gb, 0x82190290u);
            if (s_origXlTrans) PPC_LOOKUP_FUNC(gb, 0x82190290u) = shimXlateTranslate;
            s_origResAcq = PPC_LOOKUP_FUNC(gb, 0x826FAAC0u);
            if (s_origResAcq) PPC_LOOKUP_FUNC(gb, 0x826FAAC0u) = shimResAcquire;
            s_origKvBuild = PPC_LOOKUP_FUNC(gb, 0x8214D3F0u);
            if (s_origKvBuild) PPC_LOOKUP_FUNC(gb, 0x8214D3F0u) = shimKvBuild;
            s_origFsmStart = PPC_LOOKUP_FUNC(gb, 0x82360B00u);
            if (s_origFsmStart) PPC_LOOKUP_FUNC(gb, 0x82360B00u) = shimFsmStart;
            s_origActRunA = PPC_LOOKUP_FUNC(gb, 0x82293108u);
            if (s_origActRunA) PPC_LOOKUP_FUNC(gb, 0x82293108u) = shimActorRunA;
            s_origActRunB = PPC_LOOKUP_FUNC(gb, 0x822931C0u);
            if (s_origActRunB) PPC_LOOKUP_FUNC(gb, 0x822931C0u) = shimActorRunB;
            s_origStEnter = PPC_LOOKUP_FUNC(gb, 0x822918B0u);
            if (s_origStEnter) PPC_LOOKUP_FUNC(gb, 0x822918B0u) = shimStEnter;
            s_origStExit = PPC_LOOKUP_FUNC(gb, 0x822919D0u);
            if (s_origStExit) PPC_LOOKUP_FUNC(gb, 0x822919D0u) = shimStExit;
            s_origStShow = PPC_LOOKUP_FUNC(gb, 0x82291798u);
            if (s_origStShow) PPC_LOOKUP_FUNC(gb, 0x82291798u) = shimStShow;
            s_origSplashAdv = PPC_LOOKUP_FUNC(gb, 0x822A8C68u);
            if (s_origSplashAdv) PPC_LOOKUP_FUNC(gb, 0x822A8C68u) = shimSplashAdvance;
            s_origBootScrCtor = PPC_LOOKUP_FUNC(gb, 0x821D0D68u);
            if (s_origBootScrCtor) PPC_LOOKUP_FUNC(gb, 0x821D0D68u) = shimBootScrCtor;
            // iter-350e FE flow FSM link probes (all vtable-dispatched)
            s_origShowScr = PPC_LOOKUP_FUNC(gb, 0x82280760u);
            if (s_origShowScr) PPC_LOOKUP_FUNC(gb, 0x82280760u) = shimFeShowScreen;
            s_origSetScr = PPC_LOOKUP_FUNC(gb, 0x8218EAC8u);
            if (s_origSetScr) PPC_LOOKUP_FUNC(gb, 0x8218EAC8u) = shimFeSetScreen;
            s_origFsmCb = PPC_LOOKUP_FUNC(gb, 0x82293398u);
            if (s_origFsmCb) PPC_LOOKUP_FUNC(gb, 0x82293398u) = shimFsmParseCb;
            s_origFeEnq = PPC_LOOKUP_FUNC(gb, 0x821648B8u);
            if (s_origFeEnq) PPC_LOOKUP_FUNC(gb, 0x821648B8u) = shimFeEnqueue;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[FLOW] table-patch watch ARMED (D508=%p FB78=%p 45D0=%p 23A0=%p)",
                (void*)s_origD508, (void*)s_origFB78, (void*)s_orig45D0, (void*)s_orig23A0);
            // 2026-07-13 PRIME DIRECTIVE: auiNatDriveThread + feSmKickThread are
            // FORCING — they hand-drive ActivateUI, SetInterfaceState, movieGate,
            // FE screen-build and SM-rebuild. Off by default; the game must reach
            // its FE flow itself. (docs/NATURAL_PATH_CHARTER.md)
            if (sr::kernel::forcingAllowed()) {
                if (auiNatDriveOn())
                    std::thread(auiNatDriveThread, gb).detach();
                // iter-398e: SR_FESM_KICK=0 knocks out the kick thread under
                // the master gate — deep-load rescue bisect.
                static const bool s_feSmKick = [] {
                    const char* e = std::getenv("SR_FESM_KICK");
                    return !e || e[0] != '0';
                }();
                if (s_feSmKick)
                    std::thread(feSmKickThread, gb).detach();
                // iter-463 charter (burndown item 8): the Sound-flag zeroing
                // band-aid moved INSIDE the master gate. Its declared retirement
                // condition ("per-thread KPCR + real KeAcquire/KfAcquireSpinLock
                // landed") is met in code: kpcr.cpp SR_KPCR / SR_KE_SPINLOCK are
                // default ON. It wrote the guest's gameconfig on every natural
                // roll ("[CFG_SND]" in 79/81 + 68/69 run logs), so FMSoundSystem
                // init sub_82150A70 never built the mixers; the golden dump has
                // the flag = 1 and the mixers set. The default exe now leaves the
                // flag to the game. OPEN DEFECT it may expose: the iter-316e walk
                // wedge in a sound sub-init (XMA/XAudio completions we never
                // signal) - the audio stage (project_audio_silent) owns that fix.
            }
            // iter-463 burndown2: the Sound-flag zeroing is its OWN named gate so a diagnostic
            // can isolate it (SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF: only this
            // forcing, the ICALL fail-stop and everything else stay natural). Master off -> off.
            // iter-463 review: the log names what actually ran. Under
            // SR_FORCING_ISOLATE=SR_CFG_SOUND_OFF the FE drives are suppressed but the
            // Sound-flag zeroing RUNS - the old single line called both suppressed.
            const bool sndOff = sr::kernel::forcingAllowedFor("SR_CFG_SOUND_OFF");
            if (sndOff) {
                std::thread(cfgSoundOffThread, gb).detach();
            }
            if (!sr::kernel::forcingAllowed()) {
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[FLOW] FE-drive threads (auiNat/feSmKick) SUPPRESSED (SR_ALLOW_FORCING off "
                    "or isolated); Sound-flag zeroing (cfgSoundOffThread) %s",
                    sndOff ? "RUNNING (named forcing gate SR_CFG_SOUND_OFF)"
                           : "SUPPRESSED - natural path only");
            }
            // iter-349 (charter DIAG, read-only): SYSTEM-RUNNER TRANSITION POLL.
            // The four service descriptors ARE the runners (SVCTICK r3):
            // {+8 curSys, +0x1C/+0x20 pending transition, +0x28 applying}.
            // FeMasterTick stops after 4 ticks because its runner stops taking
            // the tick branch (SystemRunnerPumpOrTick: pending => ApplyTransition
            // instead). Golden steady state = all pendings zero. Log on change.
            std::thread([gb] {
                constexpr uint32_t kRunners[4] = {0x8295C62Cu, 0x8295C65Cu,
                                                  0x8295C690u, 0x8295C6C0u};
                auto rd = [gb](uint32_t va) -> uint32_t {
                    const uint8_t* p = gb + va;
                    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
                };
                uint32_t last[4][4] = {};
                // iter-349b: also poll the four GameKernel MANAGERS (cells
                // 0x8296D8D8/DC/E0/E4) — {run/pause bytes +0x40..0x43, period
                // +0x48, accum +0x68} = the tick-due inputs. FeMasterTick
                // stopping at 4 with healthy runners means paused, garbage
                // period, or frozen accum — this names which.
                constexpr uint32_t kMgrCells[4] = {0x8296D8D8u, 0x8296D8DCu,
                                                   0x8296D8E0u, 0x8296D8E4u};
                uint32_t lastM[4][3] = {};
                for (int tick = 0; tick < 200; ++tick) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    for (int i = 0; i < 4; ++i) {
                        uint32_t cur = rd(kRunners[i] + 8),
                                 p1 = rd(kRunners[i] + 0x1C),
                                 p2 = rd(kRunners[i] + 0x20),
                                 ap = rd(kRunners[i] + 0x28);
                        if (cur != last[i][0] || p1 != last[i][1] ||
                            p2 != last[i][2] || ap != last[i][3]) {
                            SR_LOG_BOOT(sr::LogSeverity::Warn,
                                "[RUNNER_POLL] t=%ds runner=%08X cur=%08X pend1C=%08X "
                                "pend20=%08X applying=%08X",
                                tick * 2, kRunners[i], cur, p1, p2, ap);
                            last[i][0] = cur; last[i][1] = p1;
                            last[i][2] = p2;  last[i][3] = ap;
                        }
                        uint32_t mgr = rd(kMgrCells[i]);
                        if (mgr < 0x10000u || mgr >= 0x9F000000u) continue;
                        uint32_t flags = rd(mgr + 0x40), per = rd(mgr + 0x4C),
                                 acc = rd(mgr + 0x6C);
                        // log flags/period changes; accum sampled every 20s
                        if (flags != lastM[i][0] || per != lastM[i][1] ||
                            (tick % 10 == 0 && acc != lastM[i][2])) {
                            SR_LOG_BOOT(sr::LogSeverity::Warn,
                                "[MGR_POLL] t=%ds cell=%08X mgr=%08X flags40=%08X "
                                "periodLo=%08X accumLo=%08X",
                                tick * 2, kMgrCells[i], mgr, flags, per, acc);
                            lastM[i][0] = flags; lastM[i][1] = per;
                            lastM[i][2] = acc;
                        }
                        // iter-349e: FMGameScript / Lua allocator chain —
                        // cell 0x8296D8CC -> module obj -> +4 allocator (the
                        // realloc front-end sub_82164818 reads *(D8CC+4)).
                        // Lua runs from t≈25s but the module init lands ~80s
                        // later — what allocator serves Lua in between?
                        if (i == 0) {
                            uint32_t mobj = rd(0x8296D8CCu);
                            uint32_t alo = (mobj >= 0x10000u) ? rd(mobj + 4) : 0;
                            uint32_t aw0 = (alo >= 0x10000u) ? rd(alo) : 0;
                            static uint32_t lastL[3] = {0xEE, 0xEE, 0xEE};
                            if (mobj != lastL[0] || alo != lastL[1] ||
                                aw0 != lastL[2]) {
                                SR_LOG_BOOT(sr::LogSeverity::Warn,
                                    "[LUAALLOC_POLL] t=%ds cellD8CC=%08X alloc(+4)=%08X "
                                    "alloc[0]=%08X", tick * 2, mobj, alo, aw0);
                                lastL[0] = mobj; lastL[1] = alo; lastL[2] = aw0;
                            }
                        }
                        // iter-350d: FE-UI bootstrap chain poll. Golden has
                        // UI-enable 0x8295C838=1 (set by FeMasterTick's
                        // FIRST-FRAME branch, gated on sysObj+4 byte) and the
                        // interface state machine live (mod+0x1310 client /
                        // +0x1320 state). Zero SetInterfaceState calls here =
                        // the chain never starts. Watch all links.
                        if (i == 0) {
                            uint32_t ui838 = rd(0x8295C838u);
                            uint32_t ff = rd(0x8291138Cu);
                            uint32_t mod = rd(0x8296D86Cu);
                            uint32_t sel = (mod >= 0x10000u) ? rd(mod + 0x1310) : 0;
                            uint32_t ist = (mod >= 0x10000u) ? rd(mod + 0x1320) : 0;
                            static uint32_t lastF[5] = {0xEE,0xEE,0xEE,0xEE,0xEE};
                            if (ui838 != lastF[0] || ff != lastF[1] ||
                                mod != lastF[2] || sel != lastF[3] || ist != lastF[4]) {
                                uint32_t hid = (mod >= 0x10000u) ? rd(mod + 0x1318) : 0;
                                uint32_t cvt = (sel >= 0x10000u) ? rd(sel) : 0;
                                uint32_t ctick = (cvt >= 0x10000u) ? rd(cvt + 0x10) : 0;
                                SR_LOG_BOOT(sr::LogSeverity::Warn,
                                    "[FEUI_POLL] t=%ds ui838=%08X ffByte=%08X mod=%08X sel1310=%08X state1320=%08X hide1318=%08X cvt=%08X tickFn=%08X reg=%08X scrId=%08X",
                                    tick * 2, ui838, ff, mod, sel, ist, hid, cvt, ctick,
                                    rd(0x8296DD0Cu),
                                    (sel >= 0x10000u) ? rd(sel + 0x580) : 0);
                                lastF[0]=ui838; lastF[1]=ff; lastF[2]=mod;
                                lastF[3]=sel; lastF[4]=ist;
                            }
                            // iter-350o: SCREEN REGISTRY CONTENTS poll. reg =
                            // *0x8296DD0C; vector {head=*reg, tail=*(reg+4)};
                            // stride 0xC entries {?, screenPtr@+4, ?}. Count =
                            // (tail-head)/0xC. Dump first entries' class vt +
                            // id (+0x58). Golden menu: >=1 entry incl the boot
                            // screen (id 1, vt 0x82009798). Answers: are ANY
                            // screens registered at boot, and is the boot
                            // screen among them?
                            {
                                uint32_t reg = rd(0x8296DD0Cu);
                                uint32_t head = (reg >= 0x10000u) ? rd(reg) : 0;
                                uint32_t tail = (reg >= 0x10000u) ? rd(reg + 4) : 0;
                                uint32_t cnt = (tail > head && (tail - head) < 0x10000u)
                                    ? (tail - head) / 0xC : 0;
                                static uint32_t lastReg[2] = {0xEE, 0xEE};
                                if (reg != lastReg[0] || cnt != lastReg[1]) {
                                    char ent[220]; int o = 0;
                                    for (uint32_t k = 0; k < cnt && k < 8 && o < 190; ++k) {
                                        uint32_t e = head + k * 0xC;
                                        uint32_t sp = rd(e + 4);
                                        o += snprintf(ent + o, sizeof(ent) - o,
                                            " [%u:vt=%08X id=%08X]", k,
                                            (sp >= 0x10000u) ? rd(sp) : 0,
                                            (sp >= 0x10000u) ? rd(sp + 0x58) : 0);
                                    }
                                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                                        "[SCRREG_POLL] t=%ds reg=%08X count=%u%s",
                                        tick * 2, reg, cnt, cnt ? ent : " (empty)");
                                    lastReg[0] = reg; lastReg[1] = cnt;
                                }
                            }
                        }
                        // iter-350e: FE flow FSM globals. e114/e118 = FSM
                        // actor stack begin/end (count = (e118-e114)/4);
                        // e0ec = FE flow allocator; e0e8 = current shown
                        // screen-state; e8bc/e8b8 = parser cur-state/action
                        // indices; e8c8 = FSM table. All static-zero here =
                        // the FE FSM config is never parsed/instanced.
                        if (i == 1) {
                            uint32_t aB = rd(0x8296E114u), aE = rd(0x8296E118u);
                            uint32_t alc = rd(0x8296E0ECu), cur = rd(0x8296E0E8u);
                            uint32_t tbl = rd(0x8296E8C8u), sI = rd(0x8296E8BCu);
                            static uint32_t lastX[6] = {0xEE,0xEE,0xEE,0xEE,0xEE,0xEE};
                            if (aB != lastX[0] || aE != lastX[1] || alc != lastX[2] ||
                                cur != lastX[3] || tbl != lastX[4] || sI != lastX[5]) {
                                SR_LOG_BOOT(sr::LogSeverity::Warn,
                                    "[FEFSM_POLL] t=%ds stack=%08X..%08X (n=%u) alloc=%08X curState=%08X tbl=%08X stIdx=%08X actIdx=%08X",
                                    tick * 2, aB, aE, (aE > aB) ? (aE - aB) / 4 : 0,
                                    alc, cur, tbl, sI, rd(0x8296E8B8u));
                                lastX[0]=aB; lastX[1]=aE; lastX[2]=alc;
                                lastX[3]=cur; lastX[4]=tbl; lastX[5]=sI;
                            }
                        }
                        // iter-350f: ResourceScheduler + flow-mgr state.
                        // sched DAT_829A2424 (vt 0x8208A5C8) first words =
                        // queue heads/counts; DAT_829A2B98 = streaming I/O
                        // list head; DAT_8296DF74 = world-flow mgr obj.
                        // iter-350i: the ctor (sub_826F4528) inits TWO item
                        // vectors at sched+0x3C and +0x48 ({begin,end,cap}).
                        // Depth = (end-begin)/4. Nonzero depth at steady
                        // state = parked items starving (no pump).
                        if (i == 2) {
                            uint32_t sch = rd(0x829A2424u);
                            uint32_t qa0 = (sch >= 0x10000u) ? rd(sch + 0x3C) : 0;
                            uint32_t qa1 = (sch >= 0x10000u) ? rd(sch + 0x40) : 0;
                            uint32_t qb0 = (sch >= 0x10000u) ? rd(sch + 0x48) : 0;
                            uint32_t qb1 = (sch >= 0x10000u) ? rd(sch + 0x4C) : 0;
                            uint32_t fm = rd(0x8296DF74u);
                            uint32_t dA = (qa1 > qa0) ? (qa1 - qa0) / 4 : 0;
                            uint32_t dB = (qb1 > qb0) ? (qb1 - qb0) / 4 : 0;
                            static uint32_t lastS[3] = {0xEE,0xEE,0xEE};
                            if (dA != lastS[0] || dB != lastS[1] || fm != lastS[2]) {
                                SR_LOG_BOOT(sr::LogSeverity::Warn,
                                    "[RSCHED_POLL] t=%ds sched=%08X qA=%u (%08X..%08X) qB=%u (%08X..%08X) flowMgr=%08X",
                                    tick * 2, sch, dA, qa0, qa1, dB, qb0, qb1, fm);
                                lastS[0]=dA; lastS[1]=dB; lastS[2]=fm;
                            }
                        }
                        // iter-350c: PSM delegate-publication poll. The
                        // splash advance runs ONLY via the PSM dispatcher
                        // (FUN_822AB748), reached through the delegate slot
                        // *(*(psm))+4 published by the PSM full ctor, gated
                        // on psm+0x230 >= 0. Never fires now (0/4). Watch
                        // the whole publication chain + the EDF8/EDFC cells.
                        if (i == 3) {
                            uint32_t psm = rd(0x8296EDACu);
                            uint32_t sub = (psm >= 0x10000u) ? rd(psm) : 0;
                            uint32_t slot = (sub >= 0x10000u) ? rd(sub + 4) : 0;
                            uint32_t c230 = (psm >= 0x10000u) ? rd(psm + 0x230) : 0;
                            uint32_t e8 = rd(0x8296EDF8u), ec = rd(0x8296EDFCu);
                            // iter-350l: + renderer-ready byte (+0x66 of
                            // DAT_829A9384) — the splash bootstrap gate.
                            uint32_t rnd = rd(0x829A9384u);
                            uint32_t r66 = (rnd >= 0x10000u)
                                ? ((rd(rnd + 0x64) >> 8) & 0xFF) : 0xEE;
                            static uint32_t lastP[6] = {0xEE,0xEE,0xEE,0xEE,0xEE,0xEE};
                            if (psm != lastP[0] || slot != lastP[1] ||
                                c230 != lastP[2] || e8 != lastP[3] || ec != lastP[4] ||
                                r66 != lastP[5]) {
                                SR_LOG_BOOT(sr::LogSeverity::Warn,
                                    "[PSM_POLL] t=%ds psm=%08X sub=%08X slot4=%08X ctr230=%08X EDF8=%08X EDFC=%08X rnd=%08X r66=%02X",
                                    tick * 2, psm, sub, slot, c230, e8, ec, rnd, r66);
                                lastP[0]=psm; lastP[1]=slot; lastP[2]=c230;
                                lastP[3]=e8;  lastP[4]=ec; lastP[5]=r66;
                            }
                        }
                        // iter-350b: FMV SM DIRECT POLL — the FMV_SM shim is a
                        // TABLE patch (indirect calls only); FeMasterTick's
                        // per-tick call is a direct bl = invisible. Truth =
                        // the SM object itself: *(0x829761E0) -> {+4 state,
                        // +8.. queue/pending}. Log on change.
                        if (i == 2) {
                            uint32_t sm = rd(0x829761E0u);
                            if (sm >= 0x10000u) {
                                uint32_t st = rd(sm + 4), q1 = rd(sm + 8),
                                         q2 = rd(sm + 0xC), q3 = rd(sm + 0x10);
                                static uint32_t lastS[4] = {0xEE, 0xEE, 0xEE, 0xEE};
                                if (st != lastS[0] || q1 != lastS[1] ||
                                    q2 != lastS[2] || q3 != lastS[3]) {
                                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                                        "[FMVSM_POLL] t=%ds sm=%08X state=%u +8=%08X +C=%08X +10=%08X",
                                        tick * 2, sm, st, q1, q2, q3);
                                    lastS[0] = st; lastS[1] = q1;
                                    lastS[2] = q2; lastS[3] = q3;
                                }
                            }
                        }
                        // iter-349g: Lua global_State forensics. L is at a
                        // stable phys VA across runs (deterministic bump);
                        // g = *(L+0x10). g's scratch buffer holds the wild
                        // {0x827245C4, 0x17FA00} pair the GC shrink-frees
                        // (pool poison) — catch its birth value + mutations.
                        if (i == 1) {
                            const uint32_t L = 0x84434624u;
                            uint32_t g = rd(L + 0x10);
                            if (g >= 0x10000u && g < 0x9F000000u) {
                                static uint32_t lastG[8] = {};
                                uint32_t w[8];
                                bool diff = false;
                                for (int k = 0; k < 8; ++k) {
                                    w[k] = rd(g + 0x10 + k * 4);
                                    if (w[k] != lastG[k]) diff = true;
                                }
                                if (diff) {
                                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                                        "[LUAG_POLL] t=%ds g=%08X +10..2C: %08X %08X %08X %08X %08X %08X %08X %08X",
                                        tick * 2, g, w[0], w[1], w[2], w[3],
                                        w[4], w[5], w[6], w[7]);
                                    memcpy(lastG, w, sizeof(lastG));
                                }
                            }
                        }
                        // iter-349c: follow the tick dispatch chain the pump
                        // actually uses — cell(runner+8) -> sysObj -> vt ->
                        // tickFn(vt+0x14). Recursion (depth 11K in Run) means
                        // some tickFn resolves back to Run 0x82720B50 — this
                        // names WHICH system and WHAT object got swapped in.
                        {
                            uint32_t cell = rd(kRunners[i] + 8);
                            uint32_t sys = (cell >= 0x10000u) ? rd(cell) : 0;
                            uint32_t svt = (sys >= 0x10000u) ? rd(sys) : 0;
                            uint32_t tfn = (svt >= 0x10000u) ? rd(svt + 0x14) : 0;
                            static uint32_t lastT[4][3] = {};
                            if (sys != lastT[i][0] || svt != lastT[i][1] ||
                                tfn != lastT[i][2]) {
                                SR_LOG_BOOT(tfn == 0x82720B50u
                                                ? sr::LogSeverity::Error
                                                : sr::LogSeverity::Warn,
                                    "[SYS_POLL] t=%ds runner=%08X cell=%08X sys=%08X "
                                    "vt=%08X tickFn=%08X%s",
                                    tick * 2, kRunners[i], cell, sys, svt, tfn,
                                    tfn == 0x82720B50u
                                        ? "  <<TICKFN==RUN (recursion source!)" : "");
                                lastT[i][0] = sys; lastT[i][1] = svt;
                                lastT[i][2] = tfn;
                            }
                        }
                    }
                }
            }).detach();
            for (int pass = 1; pass <= 3; ++pass) {
                std::this_thread::sleep_for(std::chrono::seconds(pass == 1 ? 60 : 90));
                dumpRunnerCensus(gb, pass);
            }
        }).detach();
    }
};
FlowWatchStarter s_flowWatchStarter;

} // namespace

// iter-365e PRELOAD-PIPELINE PROBE (log-only, ungated): the FE-tick death traces to
// EFFECT-class preload names (FLAGCLOTHTEST etc.) that never resolve; the vxd VFX
// definition files that would REGISTER those names are never looked up (zero '_vxd'
// lookups all run). These hooks show, in one run, (a) every parsed preloadDefs entry
// with its resource type (VFX_XML=2 per ResourceTypeFromString_821F5378), and
// (b) every preload-group loader creation — so the point where vxd entries drop out
// names itself.
extern "C" void __imp__sub_821F55B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_821F55B0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t entry = ctx.r3.u32;
  __imp__sub_821F55B0(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 400 && entry >= 0x10000u && entry < 0xC0000000u) {
    const uint32_t typeId = sr::kernel::readGuestU32BE(entry + 8u);
    const uint32_t typeSub = sr::kernel::readGuestU32BE(entry + 12u);
    // name: string field at entry+20 — may be an inline buffer or a ptr; try both.
    char nm[48] = {0};
    const uint8_t* h = sr::kernel::guestVAToHost(entry + 20u);
    if (h && h[0] >= 0x20 && h[0] < 0x7F) {
      for (size_t i = 0; i + 1 < sizeof(nm) && h[i] >= 0x20 && h[i] < 0x7F; ++i) nm[i] = (char)h[i];
    } else {
      const uint32_t p = sr::kernel::readGuestU32BE(entry + 20u);
      const uint8_t* hp = (p >= 0x10000u && p < 0xC0000000u) ? sr::kernel::guestVAToHost(p) : nullptr;
      if (hp) for (size_t i = 0; i + 1 < sizeof(nm) && hp[i] >= 0x20 && hp[i] < 0x7F; ++i) nm[i] = (char)hp[i];
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[PRELOAD_DEF] #%u entry=%08X type=%u sub=%08X name='%s' tempFlag=%u async=%u",
      n, entry, typeId, typeSub, nm,
      sr::kernel::readGuestU32BE(entry + 16u),
      (unsigned)(sr::kernel::readGuestU32BE(entry + 152u) >> 24));
  }
}

extern "C" void __imp__sub_821835E0(PPCContext& __restrict ctx, uint8_t* base);
void sub_821835E0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40) {
    char nm[40] = {0};
    const uint32_t p = ctx.r4.u32;
    const uint8_t* hp = (p >= 0x10000u && p < 0xC0000000u) ? sr::kernel::guestVAToHost(p) : nullptr;
    if (hp) for (size_t i = 0; i + 1 < sizeof(nm) && hp[i]; ++i) nm[i] = (char)hp[i];
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[PRELOAD_GRP] #%u this=%08X group='%s' r5=%u lr=%08X",
      n, ctx.r3.u32, nm, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_821835E0(ctx, base);
}

// iter-365g CATALOG-BUILD CENSUS (log-only, ungated): every AST catalog entry
// insert goes through AptCatalogInsertHashedKey_sub_826E79B0 (gated on catalog
// state +528==2 "loading"). Count inserts per catalog object and flag the
// world-effect names the FE-death request needs (FLAGCLOTHTEST-class) — this
// decides "metropol catalog build truncated" vs "name is not a catalog entry
// anywhere (console completes the not-found gracefully)".
extern "C" void __imp__sub_826E79B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826E79B0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t cat = ctx.r3.u32;
  const uint32_t namePtr = ctx.r5.u32;
  char nm[48] = {0};
  const uint8_t* hp = (namePtr >= 0x10000u && namePtr < 0xC0000000u)
                    ? sr::kernel::guestVAToHost(namePtr) : nullptr;
  if (hp) for (size_t i = 0; i + 1 < sizeof(nm) && hp[i] >= 0x20 && hp[i] < 0x7F; ++i) nm[i] = (char)hp[i];

  // per-catalog counters (fixed open-addressed slots; ~40 catalogs exist)
  static std::atomic<uint32_t> s_catVA[64];
  static std::atomic<uint32_t> s_catCount[64];
  unsigned c = 0;
  for (int i = 0; i < 64; ++i) {
    uint32_t expect = 0;
    if (s_catVA[i].load(std::memory_order_relaxed) == cat ||
        s_catVA[i].compare_exchange_strong(expect, cat, std::memory_order_relaxed)) {
      c = s_catCount[i].fetch_add(1, std::memory_order_relaxed) + 1;
      break;
    }
  }
  bool special = nm[0] &&
    (strcmp(nm, "FLAGCLOTHTEST") == 0 || strcmp(nm, "SMCAPEFX") == 0 ||
     strcmp(nm, "errorMissingFX") == 0 || strncmp(nm, "WATER_", 6) == 0);
  if (c <= 3 || (c % 1000 == 0) || special) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[CAT_INS] cat=%08X n=%u name='%s'%s lr=%08X",
      cat, c, nm, special ? " ***TARGET***" : "", (uint32_t)ctx.lr);
  }
  __imp__sub_826E79B0(ctx, base);
  if (special) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[CAT_INS] ***TARGET*** '%s' insert rc=%d cat=%08X", nm, (int)ctx.r3.s32, cat);
  }
}
