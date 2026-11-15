// iter-298: RTAM async SERVICE LAYER — the trunk fix for the
// "never-initialized subsystem" bug class (see memory
// project_async_trunk_hypothesis + ENGINE_MAP).
//
// Golden Xenia runs persistent game worker threads (ResStorageManager, OnLoad)
// that service the resource pipeline; our boot never spawns them, so no async
// asset load ever COMPLETES (bytes land, finalize work items never run,
// done-flags stay 0) and every downstream subsystem starves. Prior one-shot
// drives (iter-283/286 SR_DRIVE_LOADPKG/STREAMPUMP) poked the pump inline —
// this file instead stands in for the missing workers with persistent,
// wall-clock service threads (HARD RULE: never clock retries off pump ticks),
// then performs ONE natural package load end-to-end.
//
// Pipeline contract (Ghidra-verified 2026-07-05, renames saved):
//   RtamPackageManager  [0x8295F0E4] vt 0x82088CF8, slot3 CreatePackage
//                       = RtamPkgMgrCreatePackage_sub_826D91F0(mgr,name,listener,flat)
//   RtamPackage         vt 0x82088B80: +0xC ids[] (8B entries, word0=assetId),
//                       +0x10 count, slot5 RequestAssetAsync_sub_826D97A0(pkg,id,recv)
//   resMgr              [0x829A2434] slot7(+0x1C) worker sub_826FAAC0
//   ResourceScheduler   [0x829A2424] slot7(+0x1C) pump-once (1 = more work)
//   streaming FSM       sub_8271F848 (zero-arg, self-pumping over 0x829A2B98)
//   completion          RtamPkgLoadCbOnComplete_sub_826DAC20 (vt-dispatched)
//                       -> wrapper (vt 0x82088B44) -> receiver->vt[3](recv,wrap,st)
//   translators         acquire sub_826D9BB8 (idempotent), array [0x829A23F0],
//                       keyed map [0x8295F130] distributed by FMUI
//
// Gates (default ON; =0 disables): SR_RTAM_SCHED (scheduler pump thread),
// SR_RTAM_WORKER (resMgr worker thread), SR_RTAM_FSM (streaming FSM thread),
// SR_RTAM_LOAD (the natural ScrPressStart package load).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "guest_call.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "r1_hooks.hpp"  // R1 (docs/SPLICE_CONVERSION.md): ports of the old splice bodies

#include "sr_oracle.hpp"

#if SR_ORACLE
extern "C" void __imp__sub_826DAC20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826FAA20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82703EA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DAB88(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DAB70(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DAAE8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DA550(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DA530(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DA7F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826DA830(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826E5868(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826E4DB0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D2690(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D7850(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D5FE8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826F6208(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8214BC88(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8256B3C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D91F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D97A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D9BB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8271F848(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826FAAC0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kSchedVA   = 0x829A2424u;
constexpr uint32_t kResMgrVA  = 0x829A2434u;
constexpr uint32_t kPkgMgrVA  = 0x8295F0E4u;
constexpr uint32_t kTransVA   = 0x829A23F0u;
constexpr uint32_t kAllocVA   = 0x829A23E4u;
constexpr uint32_t kTransMapVA = 0x8295F130u;
constexpr uint32_t kNoopFnVA  = 0x8231C140u;  // FM descriptor shared no-op

// Expected vtables (structural type markers — HARD RULE: ranges pass garbage).
constexpr uint32_t kPkgMgrVt  = 0x82088CF8u;
constexpr uint32_t kSchedVt   = 0x8208A5C8u;
constexpr uint32_t kResMgrVt  = 0x8208AFF8u;

// Host latch functions registered at unused MID-FUNCTION guest VAs: only
// function starts occupy dispatch-table slots, so these slots are free. The
// guest's vt dispatch (bctrl -> PPC_LOOKUP_FUNC) lands directly in host code.
constexpr uint32_t kListenerLatchVA = kNoopFnVA + 4u;   // 0x8231C144
constexpr uint32_t kReceiverLatchVA = kNoopFnVA + 8u;   // 0x8231C148

bool gateOn(const char* name)
{
    // 2026-07-13 PRIME DIRECTIVE: every gate in this TU is a FORCING drive
    // (synthetic RTAM service threads / hand-driven loads). Master gate
    // SR_ALLOW_FORCING (default OFF) required. (docs/NATURAL_PATH_CHARTER.md)
    if (!sr::kernel::forcingAllowed()) return false;
    const char* e = std::getenv(name);
    return !e || e[0] != '0';
}

bool va_ok(uint32_t v) { return v >= 0x10000u && v < 0xC0000000u; }

// singleton readiness: pointer aligned, in range, with the EXACT expected vt
bool singletonReady(uint32_t slotVA, uint32_t wantVt)
{
    uint32_t p = sr::kernel::readGuestU32BE(slotVA);
    if (!va_ok(p) || (p & 3u)) return false;
    return sr::kernel::readGuestU32BE(p) == wantVt;
}

// Poll until guest memory exists (service threads start at static-init time).
uint8_t* waitGuestBase()
{
    for (;;) {
        uint8_t* sb = nullptr; size_t ss = 0;
        sr::kernel::getGlobalGuestMemory(&sb, &ss);
        if (sb) return sb;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// iter-306 (init-race trunk, workflow wf_4d1d0fff): hardware serializes boot by
// ORDER — Init2 runs all four StartService_* E928 init-graph walks INLINE and
// only then creates the GameKernelLoop tick threads (StartLoopThread x4, the
// only writer of the started byte mgr+0x40). Our FSM thread used to fire
// sub_8271F848 (a SECOND E630 init-walker + FE tick) ~0.4s into the sim walk
// (its resMgr/sched readiness comes true mid-core-walk), stretching the sim
// walk 10s->74s and opening the scribble window behind the NULL-roots
// AddRefNodes storm (78-712M calls, lr 0x8271F28C). Hold every harness
// guest-calling drive until the game's OWN end-of-Init2 signal: all four
// GameKernelMgr globals non-null AND their started byte +0x40 == 1.
// SR_RTAM_HOLD_UNTIL_INIT2=0 keeps the A/B.
constexpr uint32_t kGkmCoreVA   = 0x8296D8D8u;
constexpr uint32_t kGkmSimVA    = 0x8296D8E0u;
constexpr uint32_t kGkmRenderVA = 0x8296D8DCu;
constexpr uint32_t kGkmSoundVA  = 0x8296D8E4u;
bool init2Complete()
{
    for (uint32_t slot : { kGkmCoreVA, kGkmSimVA, kGkmRenderVA, kGkmSoundVA }) {
        uint32_t mgr = sr::kernel::readGuestU32BE(slot);
        if (!va_ok(mgr)) return false;
        // started byte mgr+0x40 (u8, BE irrelevant for single byte): written
        // ONLY by GameKernelMgr_StartLoopThread_sub_82720590, strictly after
        // all four E928 walks return. Non-null mgr alone is satisfiable on
        // bailed rolls — the +0x40==1 sub-condition is load-bearing.
        uint32_t w = sr::kernel::readGuestU32BE(mgr + 0x40u);
        if (((w >> 24) & 0xFFu) != 1u) return false;
    }
    return true;
}
void waitInit2Complete(const char* who)
{
    static const bool s_hold = [] {
        const char* e = std::getenv("SR_RTAM_HOLD_UNTIL_INIT2");
        return !e || e[0] != '0';   // default ON; =0 keeps the A/B
    }();
    if (!s_hold) return;
    bool logged = false;
    while (!init2Complete()) {
        if (!logged) {
            logged = true;
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_SVC] %s holding until Init2 completes (all 4 GameKernelMgr started)", who);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (logged)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_SVC] %s released — Init2 complete", who);
}

std::atomic<uint32_t> g_completeCount{0};
std::atomic<uint32_t> g_schedWork{0};
std::atomic<uint32_t> g_pkgLatch{0};
std::atomic<uint32_t> g_pkgOkLatch{0xFFFFFFFFu};
std::atomic<uint32_t> g_wrapLatch{0};
std::atomic<uint32_t> g_wrapCount{0};

// listener->vt[3](listener, pkg, ok) — CreatePackage returns VOID; the package
// arrives ONLY here (via RtamPackageBind).
void listenerLatchFn(PPCContext& __restrict ctx, uint8_t* /*base*/)
{
    g_pkgLatch.store(ctx.r4.u32, std::memory_order_relaxed);
    g_pkgOkLatch.store(ctx.r5.u32, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAM_LOAD] *** listener delivery: pkg=%08X ok=%08X ***",
        ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u32 = 0;
}

// receiver->vt[3](recv, wrapper, status) — per-asset delivery sink.
void receiverLatchFn(PPCContext& __restrict ctx, uint8_t* /*base*/)
{
    const uint32_t n = g_wrapCount.fetch_add(1, std::memory_order_relaxed) + 1;
    g_wrapLatch.store(ctx.r4.u32, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAM_LOAD] *** ASSET DELIVERED #%u: wrapper=%08X status=%08X vt=%08X ***",
        n, ctx.r4.u32, ctx.r5.u32,
        (ctx.r4.u32 >= 0x10000u && ctx.r4.u32 < 0xC0000000u)
            ? sr::kernel::readGuestU32BE(ctx.r4.u32) : 0);
    ctx.r3.u32 = 0;
}

void registerLatches(uint8_t* gb)
{
    // dispatch table is host-owned; mid-function VAs have free slots
    PPC_LOOKUP_FUNC(gb, kListenerLatchVA) = listenerLatchFn;
    PPC_LOOKUP_FUNC(gb, kReceiverLatchVA) = receiverLatchFn;
}

// ---- scheduler pump thread (stands in for the cooperative waiter pumping) --
void schedPumpMain()
{
    if (!gateOn("SR_RTAM_SCHED")) return;
    uint8_t* gb = waitGuestBase();
    while (!singletonReady(kSchedVA, kSchedVt))
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // iter-307: hold REMOVED for the sched pump — unlike fsmMain it never
    // walks the init graph (only sched->vt[7]/[8] pumping), and iter-298
    // proved natural boot items ride this scheduler DURING init; holding it
    // starves init-era poll-waiters (post-hold rolls pinned at walksRet=2
    // with a poll-cycle wedge in the physics singleton chain). fsmMain (the
    // actual second-walker/corruptor) stays held.
    sr::kernel::GuestCaller gc("RTAM_SCHED");
    if (!gc.ok()) return;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAM_SVC] scheduler pump thread up (sched=%08X)",
        sr::kernel::readGuestU32BE(kSchedVA));
    uint32_t bursts = 0;
    for (;;) {
        uint32_t sched = sr::kernel::readGuestU32BE(kSchedVA);
        uint32_t vt = va_ok(sched) ? sr::kernel::readGuestU32BE(sched) : 0;
        // The natural waiters pump SLOT8 (+0x20, sub_826F4700) with r4=1 —
        // v7b proof: our enqueued items never executed while this thread
        // pumped slot7 only. Pump slot8 (primary) and slot7 (secondary).
        uint32_t fn8VA = va_ok(vt) ? sr::kernel::readGuestU32BE(vt + 0x20u) : 0;
        uint32_t fn7VA = va_ok(vt) ? sr::kernel::readGuestU32BE(vt + 0x1Cu) : 0;
        PPCFunc* fn8 = fn8VA ? PPC_LOOKUP_FUNC_SAFE(gb, fn8VA) : nullptr;
        PPCFunc* fn7 = fn7VA ? PPC_LOOKUP_FUNC_SAFE(gb, fn7VA) : nullptr;
        if (fn8) {
            for (int i = 0; i < 64; ++i) {
                sr::kernel::GuestArgs a{};
                a.r3 = sched; a.r4 = 1;
                uint32_t exc = 0;
                if (!gc.call(fn8, a, &exc)) {
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[RTAM_SVC] sched pump8 exception 0x%08X — thread stops",
                        (unsigned)exc);
                    return;
                }
                if (gc.ctx().r3.u32 == 0u) break;
                g_schedWork.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (fn7) {
            sr::kernel::GuestArgs a{};
            a.r3 = sched;
            uint32_t exc = 0;
            if (gc.call(fn7, a, &exc) && gc.ctx().r3.u32 == 1u)
                g_schedWork.fetch_add(1, std::memory_order_relaxed);
        }
        ++bursts;
        if ((bursts & 0x3FFu) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_SVC] sched pump alive: bursts=%u worked=%u completes=%u",
                bursts, g_schedWork.load(std::memory_order_relaxed),
                g_completeCount.load(std::memory_order_relaxed));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// ---- resMgr worker thread (stands in for golden's ResStorageManager) ------
void workerMain()
{
    // iter-298 v5: sub_826FAAC0 is NOT a worker loop — it's the per-request
    // processor, called synchronously INSIDE the enqueue (sub_826FAA20 ->
    // resMgr->vt[7](resMgr, request, proc)). Calling it with request=0 was
    // a garbage call. OPT-IN only for experiments.
    const char* e = std::getenv("SR_RTAM_WORKER");
    if (!e || e[0] != '1') return;
    if (!gateOn("SR_RTAM_WORKER")) return;
    waitGuestBase();
    while (!singletonReady(kResMgrVA, kResMgrVt))
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // settle: let the singleton finish construction
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sr::kernel::GuestCaller gc("RTAM_WORKER");
    if (!gc.ok()) return;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAM_SVC] resMgr worker thread up (resMgr=%08X)",
        sr::kernel::readGuestU32BE(kResMgrVA));
    uint32_t spins = 0;
    for (;;) {
        uint32_t mgr = sr::kernel::readGuestU32BE(kResMgrVA);
        if (va_ok(mgr)) {
            sr::kernel::GuestArgs a{};
            a.r3 = mgr;
            uint32_t exc = 0;
            if (!gc.call(&__imp__sub_826FAAC0, a, &exc)) {
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[RTAM_SVC] worker exception 0x%08X — thread stops",
                    (unsigned)exc);
                return;
            }
            ++spins;
            if (spins <= 2 || (spins & 0x3FFu) == 0)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[RTAM_SVC] worker pass #%u returned r3=%08X", spins,
                    gc.ctx().r3.u32);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---- streaming FSM thread (device I/O drain; wedge-prone inline — own thread)
void fsmMain()
{
    if (!gateOn("SR_RTAM_FSM")) return;
    waitGuestBase();
    // start once the FM/resource era begins (structural: real resMgr + sched)
    while (!singletonReady(kResMgrVA, kResMgrVt) ||
           !singletonReady(kSchedVA, kSchedVt))
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // iter-306 THE CRITICAL HOLD: F848 is a second init-walker + FE tick;
    // running it during the init walks was the storm trunk (see init2Complete).
    waitInit2Complete("FSM");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sr::kernel::GuestCaller gc("RTAM_FSM");
    if (!gc.ok()) return;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[RTAM_SVC] streaming FSM thread up");
    uint32_t passes = 0;
    for (;;) {
        sr::kernel::GuestArgs a{};
        uint32_t exc = 0;
        if (!gc.call(&__imp__sub_8271F848, a, &exc)) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[RTAM_SVC] FSM exception 0x%08X — thread stops", (unsigned)exc);
            return;
        }
        ++passes;
        if (passes <= 2 || (passes & 0x3FFu) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_SVC] FSM pass #%u", passes);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ---- the natural package load ----------------------------------------------
// Guest callback object: no-op vt except slot3 = the given latch VA.
uint32_t makeLatchObj(uint32_t& vtOut, uint32_t slot3VA)
{
    if (!vtOut) {
        vtOut = sr::kernel::guestHeapAlloc(0x40);
        if (!vtOut) return 0;
        for (uint32_t i = 0; i < 14; ++i)
            sr::kernel::writeGuestBE32(vtOut + i * 4u, kNoopFnVA);
        sr::kernel::writeGuestBE32(vtOut + 3u * 4u, slot3VA);
    }
    uint32_t obj = sr::kernel::guestHeapAlloc(0x20);
    if (!obj) return 0;
    for (uint32_t i = 0; i < 8; ++i)
        sr::kernel::writeGuestBE32(obj + i * 4u, 0);
    sr::kernel::writeGuestBE32(obj, vtOut);
    return obj;
}

// Translator prep — the piece of the synthetic drive that PROVED necessary
// (registry lookups fail without it): acquire the RTAM translators once the
// singletons exist. SR_RTAM_PREP=0 disables.
void prepMain()
{
    if (!gateOn("SR_RTAM_PREP")) return;
    uint8_t* gb = waitGuestBase();
    for (;;) {
        if (va_ok(sr::kernel::readGuestU32BE(kAllocVA)) &&
            singletonReady(kSchedVA, kSchedVt) &&
            singletonReady(kPkgMgrVA, kPkgMgrVt) &&
            singletonReady(0x829A2420u, 0x8208A6BCu))  // the type REGISTRY —
            break;              // acquiring before it exists registers nothing
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // iter-307: PREP hold removed with SCHED's (translator acquire is
    // idempotent + init-era safe; see iter-307 note above).
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sr::kernel::GuestCaller gc("RTAM_PREP");
    if (!gc.ok()) return;
    sr::kernel::GuestArgs a{};
    uint32_t exc = 0;
    gc.call(&__imp__sub_826D9BB8, a, &exc);
    // verify: registry lookups for the geometry/texture keys
    sr::kernel::GuestArgs g0{};
    uint32_t exc0 = 0, tGeo = 0;
    if (gc.call(&__imp__sub_826E5868, g0, &exc0) && !exc0) {
        uint32_t reg = gc.ctx().r3.u32;
        uint32_t regVt = va_ok(reg) ? sr::kernel::readGuestU32BE(reg) : 0;
        uint32_t lkVA = va_ok(regVt)
            ? sr::kernel::readGuestU32BE(regVt + 0x1Cu) : 0;
        PPCFunc* lk = lkVA ? PPC_LOOKUP_FUNC_SAFE(gb, lkVA) : nullptr;
        if (lk) {
            sr::kernel::GuestArgs la{};
            la.r3 = reg; la.r4 = 0x264923D2u;
            uint32_t e1 = 0;
            if (gc.call(lk, la, &e1) && !e1) tGeo = gc.ctx().r3.u32;
        }
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAM_PREP] translators acquired (exc=%08X) D2 lookup=%08X",
        (unsigned)exc, tGeo);

    // iter-299 NATURAL ENTRY (golden provenance trace): the real game loads a
    // screen via ONE call — AptAuxLoadAptFileByPath_sub_826D2690(path, &holder)
    // — which builds its own REAL receivers (AssetPackageLoadedCallbackImpl ->
    // AssetLoadedCallbackImpl), requests apt-data + const-file, aggregates the
    // file record + mpTextureTable, and queues it at manager+0x7C for movie
    // instantiation. Our synthetic no-op receivers could never aggregate.
    // iter-283 drove this same call WITHOUT service threads/translators — both
    // exist now. SR_RTAM_NATLOAD=0 disables.
    if (gateOn("SR_RTAM_NATLOAD")) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint32_t nameVA = sr::kernel::guestHeapAlloc(0x60);
        uint32_t holderVA = sr::kernel::guestHeapAlloc(0x20);
        uint8_t* nh = nameVA ? sr::kernel::guestVAToHost(nameVA) : nullptr;
        if (nh && holderVA) {
            std::memset(nh, 0, 0x60);
            std::memcpy(nh, "screens\\bootflow\\ScrPressStart", 31);
            for (uint32_t z = 0; z < 8; ++z)
                sr::kernel::writeGuestBE32(holderVA + z * 4u, 0);
            sr::kernel::GuestArgs na{};
            na.r3 = nameVA; na.r4 = holderVA;
            uint32_t nexc = 0;
            bool okn = gc.call(&__imp__sub_826D2690, na, &nexc);
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_NAT] load-by-path -> ret=%08X exc=%08X holder=%08X",
                okn ? gc.ctx().r3.u32 : 0, (unsigned)nexc,
                sr::kernel::readGuestU32BE(holderVA));
            for (int t = 0; t < 60; ++t) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                uint32_t hv = sr::kernel::readGuestU32BE(holderVA);
                if ((t & 3) == 0 || hv)
                    SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[RTAM_NAT] t=%ds holder=%08X completes=%u", t * 2, hv,
                        g_completeCount.load(std::memory_order_relaxed));
                if (hv) break;
            }
        }
    }
}

// Real-receiver stage watches (golden-named): package-bound and asset-delivered
// handlers of the game's own callback impls.
PPC_FUNC(sub_826D5FE8)   // AssetPackageLoadedCallbackImpl::OnPackageBound
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_NAT] OnPackageBound #%u this=%08X pkg=%08X ok=%08X",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    __imp__sub_826D5FE8(ctx, base);
}
PPC_FUNC(sub_826D7850)   // AssetLoadedCallbackImpl::OnAssetLoaded
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_NAT] *** OnAssetLoaded #%u this=%08X asset=%08X st=%08X ***",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    __imp__sub_826D7850(ctx, base);
}

void loadMain()
{
    // v8 PIVOT: the synthetic per-asset drive is OPT-IN now (4 attempts at
    // its exec/complete wall — trunk-first says test the NATURAL loadMovie
    // instead, which the service layer + translator prep may have unblocked).
    const char* le = std::getenv("SR_RTAM_LOAD");
    if (!le || le[0] != '1') return;
    uint8_t* gb = waitGuestBase();
    // preconditions: allocator + REAL (vt-verified) scheduler and pkg manager
    for (;;) {
        if (va_ok(sr::kernel::readGuestU32BE(kAllocVA)) &&
            singletonReady(kSchedVA, kSchedVt) &&
            singletonReady(kPkgMgrVA, kPkgMgrVt))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    registerLatches(gb);
    sr::kernel::GuestCaller gc("RTAM_LOAD");
    if (!gc.ok()) return;

    // Translator acquire — ALWAYS (idempotent, refcounted): a nonzero array
    // global does NOT mean the translators were REGISTERED into the type
    // registry; unregistered keys make the item ctor store translator=NULL
    // at item+0x40 and the execute's load call lands in garbage (silent
    // no-op — the v6 "loads never fire" wall).
    {
        sr::kernel::GuestArgs a{};
        uint32_t exc = 0;
        gc.call(&__imp__sub_826D9BB8, a, &exc);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_LOAD] translator acquire -> trans=%08X exc=%08X",
            sr::kernel::readGuestU32BE(kTransVA), (unsigned)exc);
    }
    // Registry-lookup diag: do the Geometry/Texture keys resolve?
    {
        sr::kernel::GuestArgs g0{};
        uint32_t exc0 = 0;
        if (gc.call(&__imp__sub_826E5868, g0, &exc0) && !exc0) {
            uint32_t reg = gc.ctx().r3.u32;
            uint32_t regVt = va_ok(reg) ? sr::kernel::readGuestU32BE(reg) : 0;
            uint32_t lkVA = va_ok(regVt)
                ? sr::kernel::readGuestU32BE(regVt + 0x1Cu) : 0;
            PPCFunc* lk = lkVA ? PPC_LOOKUP_FUNC_SAFE(gb, lkVA) : nullptr;
            uint32_t tGeo = 0, tTex = 0;
            if (lk) {
                sr::kernel::GuestArgs la{};
                la.r3 = reg; la.r4 = 0x264923D2u;
                uint32_t e1 = 0;
                if (gc.call(lk, la, &e1) && !e1) tGeo = gc.ctx().r3.u32;
                la.r4 = 0x264923D3u;
                if (gc.call(lk, la, &e1) && !e1) tTex = gc.ctx().r3.u32;
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_LOAD] registry=%08X lookup: D2(geo)=%08X(vt %08X) "
                "D3(tex)=%08X(vt %08X)",
                reg, tGeo,
                va_ok(tGeo) ? sr::kernel::readGuestU32BE(tGeo) : 0,
                tTex,
                va_ok(tTex) ? sr::kernel::readGuestU32BE(tTex) : 0);
        }
    }

    uint32_t nameVA = sr::kernel::guestHeapAlloc(0x40);
    uint8_t* nh = nameVA ? sr::kernel::guestVAToHost(nameVA) : nullptr;
    if (!nh) return;
    std::memset(nh, 0, 0x40);
    std::memcpy(nh, "screens\\bootflow\\ScrPressStart", 31);

    uint32_t listenerVt = 0, receiverVt = 0;
    uint32_t mgr = 0, pkg = 0, count = 0, ids = 0;
    // The bind races the catalog/BigFile init (v2 evidence: a late attempt
    // succeeded where early ones bound count=0). Retry until it binds.
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::seconds(10));
        uint32_t listener = makeLatchObj(listenerVt, kListenerLatchVA);
        mgr = sr::kernel::readGuestU32BE(kPkgMgrVA);
        g_pkgLatch.store(0, std::memory_order_relaxed);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_LOAD] CreatePackage attempt %d (mgr=%08X listener=%08X "
            "trans=%08X map=%08X)",
            attempt, mgr, listener, sr::kernel::readGuestU32BE(kTransVA),
            sr::kernel::readGuestU32BE(kTransMapVA));
        sr::kernel::GuestArgs ca{};
        ca.r3 = mgr; ca.r4 = nameVA; ca.r5 = listener; ca.r6 = 0;
        uint32_t exc = 0;
        if (!gc.call(&__imp__sub_826D91F0, ca, &exc)) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[RTAM_LOAD] CreatePackage exception 0x%08X", (unsigned)exc);
            continue;
        }
        // CreatePackage returns VOID — the package arrives at listener->vt[3]
        // (synchronously, from RtamPackageBind). Bind failure = count 0.
        pkg = g_pkgLatch.load(std::memory_order_relaxed);
        count = va_ok(pkg) ? sr::kernel::readGuestU32BE(pkg + 0x10u) : 0;
        ids = va_ok(pkg) ? sr::kernel::readGuestU32BE(pkg + 0x0Cu) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_LOAD] attempt %d: pkg=%08X ok=%08X count=%u ids=%08X "
            "storage=%08X",
            attempt, pkg, g_pkgOkLatch.load(std::memory_order_relaxed), count,
            ids, va_ok(pkg) ? sr::kernel::readGuestU32BE(pkg + 4u) : 0);
        if (va_ok(pkg) && va_ok(ids) && count > 0 && count <= 0x200u) break;
        pkg = 0;
    }
    if (!pkg) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[RTAM_LOAD] package never bound after retries");
        return;
    }

    uint32_t nReq = 0;
    for (uint32_t i = 0; i < count && i < 16u; ++i) {
        // 8-byte id entries = {namespaceHash (0x1A53CAFC for all), assetId}.
        // Golden asset records confirm: word1 is the unique per-asset id
        // (0x2A, 0x2E, ...); word0 is shared.
        uint32_t nsHash = sr::kernel::readGuestU32BE(ids + i * 8u);
        uint32_t assetId = sr::kernel::readGuestU32BE(ids + i * 8u + 4u);
        uint32_t recv = makeLatchObj(receiverVt, kReceiverLatchVA);
        sr::kernel::GuestArgs ra{};
        ra.r3 = pkg; ra.r4 = assetId; ra.r5 = recv;
        uint32_t rexc = 0;
        bool okc = gc.call(&__imp__sub_826D97A0, ra, &rexc);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_LOAD] request[%u] ns=%08X id=%08X recv=%08X -> ret=%08X exc=%08X",
            i, nsHash, assetId, recv, okc ? gc.ctx().r3.u32 : 0, (unsigned)rexc);
        if (okc) ++nReq;
    }

    // observe: completions arrive via the service threads
    for (int t = 0; t < 120; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if ((t & 3) == 0) {
            uint32_t loaded = sr::kernel::readGuestU32BE(pkg + 0x28u);
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_LOAD] wait t=%ds requested=%u completes=%u delivered=%u "
                "tbl=%08X tbl[0..3]=%08X %08X %08X %08X",
                t * 2, nReq, g_completeCount.load(std::memory_order_relaxed),
                g_wrapCount.load(std::memory_order_relaxed), loaded,
                va_ok(loaded) ? sr::kernel::readGuestU32BE(loaded) : 0,
                va_ok(loaded) ? sr::kernel::readGuestU32BE(loaded + 4u) : 0,
                va_ok(loaded) ? sr::kernel::readGuestU32BE(loaded + 8u) : 0,
                va_ok(loaded) ? sr::kernel::readGuestU32BE(loaded + 12u) : 0);
        }
    }
}

// iter-300: drive FMGameRtamIni's init directly — it creates the
// ResourceSystem singleton (DAT_8296D8C8), the ResStorageManager thread and
// the OnLoad worker (DAT_829A25FC): the ACTUAL async service layer the whole
// asset pipeline runs on (NULL on 3/4 rolls because the natural dispatcher
// sub_8271E630 is hand-edit-skipped and the walker stalls before this
// module). Idempotent: sub_8218F5F8 checks DAT_8296D8C8 internally.
// SR_DRIVE_RTAMINI=0 disables.
void rtamIniMain()
{
    if (!gateOn("SR_DRIVE_RTAMINI")) return;
    // iter-302 burndown 1.5: the natural walk runs FMGameRtamIni (takes up
    // to ~64s) — this 5s-delayed drive races it.
    if (sr::kernel::naturalModuleInitOn()) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAMINI] drive retired (SR_NATURAL_MODULE_INIT on)");
        return;
    }
    waitGuestBase();
    // FM-kernel era must exist first
    while (!va_ok(sr::kernel::readGuestU32BE(0x829A2BF0u)))
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (va_ok(sr::kernel::readGuestU32BE(0x8296D8C8u))) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAMINI] resource system already up (%08X) — no drive needed",
            sr::kernel::readGuestU32BE(0x8296D8C8u));
        return;
    }
    sr::kernel::GuestCaller gc("RTAMINI_DRV");
    if (!gc.ok()) return;
    sr::kernel::GuestArgs a{};
    uint32_t exc = 0;
    bool ok = gc.call(&__imp__sub_8214BC88, a, &exc);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RTAMINI] FMGameRtamIni init driven: ret=%d exc=%08X resSys=%08X "
        "onLoad=%08X",
        (int)ok, (unsigned)exc, sr::kernel::readGuestU32BE(0x8296D8C8u),
        sr::kernel::readGuestU32BE(0x829A25FCu));
}

struct RtamServiceStarter {
    RtamServiceStarter()
    {
        // iter-463 charter: every thread below is a synthetic RTAM drive
        // (guest calls, guest-heap writes, dispatch-table latches) - FORCING.
        // Master-gated at the launch so a default run starts none of them;
        // the per-drive env gates (gateOn, SR_RTAM_LOAD, SR_RTAM_WORKER) stay
        // as sub-gates under the master.
        if (!sr::kernel::forcingAllowed()) return;
        std::thread(schedPumpMain).detach();
        std::thread(workerMain).detach();
        std::thread(fsmMain).detach();
        std::thread(prepMain).detach();
        std::thread(loadMain).detach();
        std::thread(rtamIniMain).detach();
    }
};
RtamServiceStarter s_rtamStarter;

// iter-302: FMPhysics init wedge site — sub_8256B3C0 is a pending-FACTORY
// retry loop (list at 0x82979558, nodes {factoryFn, next, outSlot}): factories
// returning 0 are retried FOREVER. Log the pending list on entry so a stuck
// factory names itself; the walk-thread sampler already shows the spin
// (lr 0x8256B4A4).
PPC_FUNC(sub_8256B3C0)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) {
        uint32_t node = sr::kernel::readGuestU32BE(0x82979558u);
        char buf[400]; int o = 0; int cnt = 0;
        while (va_ok(node) && cnt < 12 && o < 330) {
            uint32_t fn = sr::kernel::readGuestU32BE(node);
            uint32_t slot = sr::kernel::readGuestU32BE(node + 8u);
            uint32_t val = va_ok(slot) ? sr::kernel::readGuestU32BE(slot) : 0xDEADu;
            o += snprintf(buf + o, sizeof(buf) - o, " %08X->%08X", fn, val);
            node = sr::kernel::readGuestU32BE(node + 4u);
            ++cnt;
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FACTORY_LOOP] #%u pending(%d):%s", n, cnt, buf);
    }
    __imp__sub_8256B3C0(ctx, base);
    if (n <= 4)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[FACTORY_LOOP] #%u RETURNED", n);
}

// iter-300 guard: EASTL vector insert-fill — the FMGameRtamIni stall parks
// here (runaway fill from a garbage count / corrupted begin-end, the known
// scribble-spin class). Only intervene on INSANE inputs. SR_VECFILL_GUARD=0
// disables.
PPC_FUNC(sub_826F6208)
{
    static const bool s_guard = [] {
        const char* e = std::getenv("SR_VECFILL_GUARD");
        return !e || e[0] != '0';
    }();
    if (s_guard) {
        const uint32_t vec = ctx.r3.u32;
        const uint32_t cnt = ctx.r5.u32;
        uint32_t beg = 0, end = 0;
        if (vec >= 0x10000u && vec < 0xC0000000u) {
            beg = sr::kernel::readGuestU32BE(vec + 4u);
            end = sr::kernel::readGuestU32BE(vec + 8u);
        }
        const bool insane = cnt > 0x100000u || beg > end ||
                            (end - beg) > 0x4000000u;
        if (insane) {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8)
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[VECFILL_GUARD] #%u INSANE fill skipped: vec=%08X "
                    "beg=%08X end=%08X cnt=%08X lr=%08X",
                    n, vec, beg, end, cnt, (uint32_t)ctx.lr);
            return;   // skip the fill entirely
        }
    }
    __imp__sub_826F6208(ctx, base);
}

} // namespace

// Queue-stage watches (all cross-TU from their callers — interceptable):
// which stage do the 16 requests die at?
PPC_FUNC(sub_826FAA20)   // resMgr enqueue (vt slot9)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 6 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_Q] enqueue #%u r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    __imp__sub_826FAA20(ctx, base);
}
// (ServiceNext sub_826F4230 and item-ctor sub_82704E88 already have iter-213
// strong-defs with logging in cs_timeout_underflow_fix.cpp — use their tags:
// [826F4230_THROTTLE] shows empty/non-empty queues.)
namespace sr { namespace kernel { extern std::atomic<int> g_srInFlowInit; } }
PPC_FUNC(sub_82703EA0)   // item EXECUTE (QI cb -> translator load -> notify)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-385g [TASKFLOW] KICK: unconditional compact line — the conveyor
    // wedge analysis needs the FINAL cycle's edge, which sampling misses.
    // iter-386b: + task state pre/post (phase +0x14, cursor +0x38, owner
    // +0x44, mode +0x48, started +0x4C, cmpl +0x4F). The wedge = a fully-
    // loaded record whose instantiation was SKIPPED; suspect = concurrent
    // kicks (owner tid-49 pump vs pool tid-53 deferred kick, both run this
    // fn on the SAME task) racing these fields — the fatal exec's entry
    // state names the clobbered one.
    const uint32_t kTask = ctx.r3.u32;
    uint32_t ph0 = 0, cu0 = 0, ow0 = 0;
    uint8_t st0 = 0, cm0 = 0;
    if (kTask >= 0x10000u && kTask < 0xC0000000u) {
        ph0 = sr::kernel::readGuestU32BE(kTask + 0x14);
        cu0 = sr::kernel::readGuestU32BE(kTask + 0x38);
        ow0 = sr::kernel::readGuestU32BE(kTask + 0x44);
        if (uint8_t* h = sr::kernel::guestVAToHost(kTask + 0x4C)) st0 = *h;
        if (uint8_t* h = sr::kernel::guestVAToHost(kTask + 0x4F)) cm0 = *h;
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[TASKFLOW] KICK #%u task=%08X pre[ph=%X cu=%X ow=%X st=%u cm=%u] tid=%u lr=%08X",
        n, kTask, ph0, cu0, ow0, (unsigned)st0, (unsigned)cm0,
        sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
    struct KickExit {
        uint32_t task, n;
        ~KickExit() {
            if (task < 0x10000u || task >= 0xC0000000u) return;
            uint32_t ph = sr::kernel::readGuestU32BE(task + 0x14);
            uint32_t cu = sr::kernel::readGuestU32BE(task + 0x38);
            uint32_t ow = sr::kernel::readGuestU32BE(task + 0x44);
            uint8_t st = 0, cm = 0;
            if (uint8_t* h = sr::kernel::guestVAToHost(task + 0x4C)) st = *h;
            if (uint8_t* h = sr::kernel::guestVAToHost(task + 0x4F)) cm = *h;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[TASKFLOW] KICK #%u post[ph=%X cu=%X ow=%X st=%u cm=%u] tid=%u",
                n, ph, cu, ow, (unsigned)st, (unsigned)cm,
                sr::kernel::getCurrentThreadHandle());
        }
    } kickExit{kTask, n};
    // iter-350h: unsampled inside the WorldFlowXML-init window (the flow
    // item exec fell in the sample gap every batch).
    if (n <= 8 || (n & 0x3Fu) == 0 ||
        sr::kernel::g_srInFlowInit.load(std::memory_order_relaxed) > 0) {
        // iter-304: include the item NAME (+8) and TRANSLATOR (+0x40) — the
        // gameconfig sync load (sound-walk wall) needs to show up by name.
        const uint32_t it = ctx.r3.u32;
        char name[64] = {0};
        if (it >= 0x10000u && it < 0xC0000000u) {
            const uint32_t nameVA = sr::kernel::readGuestU32BE(it + 8);
            if (nameVA >= 0x10000u && nameVA < 0xC0000000u) {
                const uint8_t* p = base + nameVA;
                size_t i = 0;
                for (; i + 1 < sizeof(name) && p[i]; ++i)
                    name[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
                name[i] = 0;
            }
        }
        const uint32_t transl =
            (it >= 0x10000u && it < 0xC0000000u) ? sr::kernel::readGuestU32BE(it + 0x40) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_Q] item-exec #%u item=%08X name='%s' transl=%08X lr=%08X",
            n, ctx.r3.u32, name, transl, (uint32_t)ctx.lr);
    }
    // Item field dump (first 2 items): find the translator — for each
    // pointer-like field print {off, val, *val (vt candidate)}.
    if (n <= 2) {
        const uint32_t it = ctx.r3.u32;
        if (it >= 0x10000u && it < 0xC0000000u) {
            char buf[420]; int o = 0;
            for (uint32_t f = 0; f < 0x50u && o < 340; f += 4) {
                uint32_t v = sr::kernel::readGuestU32BE(it + f);
                if (v >= 0x10000u && v < 0xC0000000u && !(v & 3u)) {
                    uint32_t vt = sr::kernel::readGuestU32BE(v);
                    if (vt >= 0x82000000u && vt < 0x820A0000u)
                        o += snprintf(buf + o, sizeof(buf) - o,
                            " +%02X=%08X(vt %08X)", f, v, vt);
                }
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[RTAM_Q] item#%u fields:%s", n, buf);
        }
    }
    // Phase 4.3 batch 3 (docs/FORCING_RETIREMENT.md): the null-this entry guard of the old splice body is
    // retired; the generated body runs for every call, as on the console.
    __imp__sub_82703EA0(ctx, base);
}
// v6 stage watches: which of {QI, load-start, load-complete, dispatch} fails?
PPC_FUNC(sub_826DAB88)   // cb QueryInterface (vt 0x82088CC8 slot0)
{
    const uint32_t iid = ctx.r4.u32;
    __imp__sub_826DAB88(ctx, base);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    // ALWAYS log the completion-interface probes (sampling missed them in v6)
    if (iid == 0x20F7D7F1u || n <= 8 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] cbQI #%u iid=%08X -> rc=%08X", n, iid, ctx.r3.u32);
}
PPC_FUNC(sub_826E4DB0)   // processor branch decider: catalog-known?
{
    __imp__sub_826E4DB0(ctx, base);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] known? #%u -> %08X", n, ctx.r3.u32);
}
PPC_FUNC(sub_826DAB70)   // cb dispatch (vt slot6, tail-calls OnLoadComplete)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] cbDispatch #%u r3=%08X", n, ctx.r3.u32);
    __imp__sub_826DAB70(ctx, base);
}
PPC_FUNC(sub_826DAAE8)   // Apt/Const translator LOAD (vt 0x82088C90 slot7)
{
    const uint32_t t = ctx.r3.u32;
    __imp__sub_826DAAE8(ctx, base);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] aptLoad #%u transl=%08X -> rc=%08X", n, t, ctx.r3.u32);
}
PPC_FUNC(sub_826DA550)   // Geometry/Texture translator LOAD (vt 0x82088BB0 slot7)
{
    const uint32_t t = ctx.r3.u32;
    __imp__sub_826DA550(ctx, base);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] geoLoad #%u transl=%08X -> rc=%08X", n, t, ctx.r3.u32);
}
PPC_FUNC(sub_826DA530)   // shared translator async RE-ARM (vt slot5)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0xFFu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] re-arm #%u r3=%08X", n, ctx.r3.u32);
    __imp__sub_826DA530(ctx, base);
}
PPC_FUNC(sub_826DA7F0)   // third-family translator LOAD (vt 0x82088C34 slot7)
{
    const uint32_t t = ctx.r3.u32;
    __imp__sub_826DA7F0(ctx, base);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0x3Fu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] c34Load #%u transl=%08X -> rc=%08X", n, t, ctx.r3.u32);
}
PPC_FUNC(sub_826DA830)   // third-family async RE-ARM (vt 0x82088C34 slot5)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0xFFu) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_V6] c34re-arm #%u r3=%08X", n, ctx.r3.u32);
    __imp__sub_826DA830(ctx, base);
}

// Completion watch — vt-dispatched (cb vt 0x82088CC8 slot4), so the strong-def
// intercepts. Fields: +0xC pkg, +0x10 receiver, +0x14 asset index.
PPC_FUNC(sub_826DAC20)
{
    const uint32_t cb = ctx.r3.u32;
    const uint32_t n = g_completeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & 0x3Fu) == 0) {
        const bool ok = cb >= 0x10000u && cb < 0xC0000000u;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[RTAM_COMPLETE] #%u cb=%08X pkg=%08X recv=%08X idx=%u",
            n, cb,
            ok ? sr::kernel::readGuestU32BE(cb + 0x0Cu) : 0,
            ok ? sr::kernel::readGuestU32BE(cb + 0x10u) : 0,
            ok ? sr::kernel::readGuestU32BE(cb + 0x14u) : 0);
    }
    __imp__sub_826DAC20(ctx, base);
}

#endif  // Retired synthetic service threads and speculative observers
