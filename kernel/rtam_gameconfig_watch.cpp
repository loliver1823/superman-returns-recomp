// iter-304: gameconfig synchronous-load watch — the sound-walk wall.
//
// With the clock fixed (iter-303) the init walks run core✓ sim✓ render✓ and
// stop 4/4-deterministically in walk#4 (sound, roots 0x8295C848) inside the
// module init Function_8214AFB8 (obj 0x8295FB68): it builds the AUI config
// global DAT_8296D874 then does a SYNCHRONOUS RTAM asset load of "gameconfig"
// (type-hash 0x0EA70951, descriptor string at 0x82001AD4) through resMgr
// DAT_829A2434: vt[+0x20] sub_826FAA70 acquire → vt[+0x28] sub_826FB078 load.
// The load's waiter cooperatively pumps the scheduler (sampler: 12/20 samples
// in pump sub_826F4700) but the item never completes — the iter-298
// async-delivery wall, now load-bearing on the natural boot with a minimal
// deterministic test case. This watch logs the load lifecycle so the K-run
// states WHERE delivery dies:
//   [GC_LOAD]   sub_826FB078 entry/exit (holder + first word + return)
//   [GC_REG]    sub_826F5480 translator-registry lookup by hash (registry
//               DAT_829A2420 vt[+0x1C]) — hash + result; NULL for 0x0EA70951
//               = "translator never registered" (iter-298 v7 class)
//   [GC_EXEC]   sub_82703EA0 item execute — item, name, translator@+0x40
// SR_GC_WATCH=0 disables logging (counting stays free).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>
#include "r1_hooks.hpp"  // R1 (docs/SPLICE_CONVERSION.md): ports of the old splice bodies
#include "sr_oracle.hpp"

#if SR_ORACLE

extern "C" void __imp__sub_826FB078(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826F5480(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82703EA0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
bool gcWatchOn()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_GC_WATCH");
        return !e || e[0] != '0';   // default ON; =0 keeps the A/B
    }();
    return v;
}

inline bool plausibleVA(uint32_t va) { return va >= 0x10000u && va < 0xC0000000u; }

inline uint32_t rd32(uint8_t* base, uint32_t va)
{
    if (!plausibleVA(va)) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Copy a short guest C-string for logging (item names are ASCII paths).
inline void rdStr(uint8_t* base, uint32_t va, char* out, size_t cap)
{
    out[0] = 0;
    if (!plausibleVA(va)) return;
    const uint8_t* p = base + va;
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        uint8_t c = p[i];
        if (c == 0) break;
        out[i] = (c >= 0x20 && c < 0x7F) ? char(c) : '.';
    }
    out[i] = 0;
}
} // namespace

// Synchronous asset load (resMgr vt[+0x28]). r3 = resMgr, r4 = holder,
// r5 = callback/receiver (0 from the gameconfig init).
// iter-312: the gameconfig load exits rc=1 WITHOUT translator work because
// the record's data binding is empty (the load's *(rec+0xC)!=0 gate skips
// everything). Dump the record's first 8 words on ENTER/EXIT to name the
// unbound field; the acquire watch below shows whether binding ever happens.
PPC_FUNC(sub_826FB078)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t holder = ctx.r4.u32;
    const uint32_t rec = holder ? rd32(base, holder) : 0;
    const bool log = gcWatchOn() && (n <= 24 || (n & 0x3F) == 0);
    if (log) {
        char f[200]; int o = 0;
        for (uint32_t i = 0; rec && i < 8; ++i)
            o += snprintf(f + o, sizeof(f) - o, " +%X=%08X", i * 4, rd32(base, rec + i * 4));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[GC_LOAD] #%u ENTER mgr=%08X holder=%08X rec=%08X cb=%08X lr=%08X fields:%s",
            n, ctx.r3.u32, holder, rec, ctx.r5.u32, (uint32_t)ctx.lr, rec ? f : " -");
    }
    __imp__sub_826FB078(ctx, base);
    if (log) {
        const uint32_t recAfter = holder ? rd32(base, holder) : 0;
        char f[200]; int o = 0;
        for (uint32_t i = 0; recAfter && i < 8; ++i)
            o += snprintf(f + o, sizeof(f) - o, " +%X=%08X", i * 4, rd32(base, recAfter + i * 4));
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[GC_LOAD] #%u EXIT rc=%u rec=%08X fields:%s",
            n, ctx.r3.u32, recAfter, recAfter ? f : " -");
    }
}

// (acquire wrapper for sub_826FAA70 dropped — lld reported a duplicate-symbol
// conflict unique to that VA; the [GC_LOAD] ENTER field dump above carries the
// same information: the record's binding state at load time.)

// Translator-registry lookup by type-hash (registry DAT_829A2420 vt[+0x1C]).
// r3 = registry, r4 = type hash. Returns translator (or 0) in r3.
PPC_FUNC(sub_826F5480)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t hash = ctx.r4.u32;
    const bool interesting = (hash == 0x0EA70951u);
    __imp__sub_826F5480(ctx, base);
    if (gcWatchOn() && (interesting || n <= 24)) {
        const uint32_t transl = ctx.r3.u32;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[GC_REG] #%u hash=%08X -> translator=%08X vt=%08X%s",
            n, hash, transl, transl ? rd32(base, transl) : 0,
            interesting ? "  <-- GAMECONFIG" : "");
    }
}

// NOTE: item execute sub_82703EA0 is already strong-def'd in rtam_service.cpp
// ([RTAM_Q] item-exec) — its logging was extended there with the item NAME
// (+8) and TRANSLATOR (+0x40) for the gameconfig diagnosis instead of a
// duplicate wrapper here.

// ---------------------------------------------------------------------------
// iter-304b: the FMPhysics wedge chain (3/4 rolls, walk #4).
// PendingFactoryRetryLoop_sub_8256B3C0 spins because one of THREE statically
// registered factories (nodes 0x82929038 -> Function_825EADE8, 0x829595EC ->
// sub_828A8F10, 0x82928840 -> Function_8256A710, list head 0x82979558)
// returns 0 forever. All three bottom out in the DOGMA slab allocator
// DAT_82979460->vt[+0x10] = sub_8256CBC0(this, size), whose slab refill goes
// through the fn-ptr global DAT_82928778 = sub_822BD498 -> sub_8278D258 (the
// iter-66 bypassed DOGMA pool alloc) with pool DAT_8296F23C (copied from
// DAT_8296D89C). Factories DON'T null-check: on hardware a failed alloc
// crashes; on our writable null page it silently returns 0 -> infinite retry.
// These watches name the failing hop. All are indirect-call targets (vtable /
// bctrl / fn-ptr global), so strong-defs intercept despite the TU-binding
// gotcha.

extern "C" void __imp__sub_8271E928(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8256CBC0(PPCContext& __restrict ctx, uint8_t* base);

// iter-306 (init-race trunk, workflow wf_4d1d0fff): E928 runner-whitelist
// tripwire. run3 showed StartService_* callers passing SCRIBBLED runner
// pointers (slot 0x82911318: 0x8295C690 -> 0x82001790; slot 0x8291132C:
// 0x8295C6C0 -> 0x829113C0) into SystemRunnerStart_sub_8271E928, whose
// AddRefNodes walk then shreds memory (structural markers beat range checks —
// both scribbles are inside the static image and pass any range check). The
// four legitimate system runners are a closed set. On a garbage runner: log
// LOUDLY and skip the walk (hardware equivalent: this state never occurs; a
// wrong pointer here means the scribbler already struck — the log names the
// value and the fix hunt moves to a write-watch on 0x82911310..0x82911330).
// This guard's firing rate is the DISCRIMINATOR for the hold-until-Init2 fix:
// zero firings with the hold ON = the FSM race was the scribbler; any firing
// = scribbler elsewhere, pivot to the write-watch. The companion roots==NULL
// guard (run2 class) lives mid-function and needs the E928 SPLICE — staged,
// not included here. SR_E928_RUNNER_GUARD=0 disables.
PPC_FUNC(sub_8271E928)
{
    // iter-463 charter (burndown U1): a registered band-aid that shipped
    // default ON. "[E928_RUNNER_GUARD]" is in 0/150 run logs. It now needs
    // SR_ALLOW_FORCING (=0 still keeps the A/B under the master gate).
    static const bool s_guard = [] {
        const char* e = std::getenv("SR_E928_RUNNER_GUARD");
        return (!e || e[0] != '0') && sr::kernel::forcingAllowed();
    }();
    const uint32_t runner = ctx.r3.u32;
    if (s_guard && runner != 0x8295C62Cu && runner != 0x8295C65Cu &&
        runner != 0x8295C690u && runner != 0x8295C6C0u) {
        static std::atomic<uint32_t> s_hits{0};
        const uint32_t n = s_hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[E928_RUNNER_GUARD] #%u GARBAGE RUNNER r3=%08X lr=%08X — walk SKIPPED"
                " (scribbler struck before this call; check slots 0x82911310..30)",
                n, runner, (uint32_t)ctx.lr);
        ctx.r3.u32 = 0;
        return;
    }
    sr::r1::e928Enter();  // R1: call numbering for the E630_enter walkDepth marker
    __imp__sub_8271E928(ctx, base);
    sr::r1::e928Exit();
}

#else
// Retain the harness's walk-depth metric without speculative guest reads or
// the retired runner whitelist. The generated guest body executes once.
extern "C" void __imp__sub_8271E928(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8271E928)
{
    sr::r1::e928Enter();
    __imp__sub_8271E928(ctx, base);
    sr::r1::e928Exit();
}
#endif

#if SR_ORACLE
extern "C" void __imp__sub_8256A710(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825EADE8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828A8F10(PPCContext& __restrict ctx, uint8_t* base);

// DOGMA slab alloc (allocator vt[+0x10]). r3 = allocator, r4 = size.
PPC_FUNC(sub_8256CBC0)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool log = gcWatchOn() && (n <= 12 || (n % 200000) == 0);
    const uint32_t athis = ctx.r3.u32, size = ctx.r4.u32;
    // iter-307: >0x2000 sizes exit the slab into the DOGMA pool large path —
    // the iter-306 8/8-deterministic FMPhysics wedge is the FIRST such call
    // never returning. Log the ENTRY (a blocked call logs nothing post-call),
    // plus the allocator CS word (allocator+0x3C..) to expose a held lock.
    if (gcWatchOn() && size > 0x2000u) {
        static std::atomic<uint32_t> s_big{0};
        const uint32_t b = s_big.fetch_add(1, std::memory_order_relaxed) + 1;
        if (b <= 24)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DGA] BIG-alloc ENTER #%u this=%08X size=%u backing=%08X csWord=%08X lr=%08X",
                b, athis, size, rd32(base, 0x82928778),
                rd32(base, athis + 0x3C), (uint32_t)ctx.lr);
    }
    __imp__sub_8256CBC0(ctx, base);
    if (log || (ctx.r3.u32 == 0 && gcWatchOn())) {
        static std::atomic<uint32_t> s_zero{0};
        const uint32_t z = (ctx.r3.u32 == 0)
            ? s_zero.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (log || z <= 12 || (z % 200000) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DGA] slab-alloc #%u this=%08X size=%u -> %08X (zeros=%u) backing=%08X poolF23C=%08X poolD89C=%08X lr=%08X",
                n, athis, size, ctx.r3.u32, z, rd32(base, 0x82928778),
                rd32(base, 0x8296F23C), rd32(base, 0x8296D89C), (uint32_t)ctx.lr);
    }
}

// iter-307: the slab's page-backing fn (fn-ptr global DAT_82928778 -> here ->
// sub_8278D258 with pool DAT_8296F23C). Entry/exit pair: ENTER without EXIT
// pins the block inside 8278D258/pool-vtcall; no ENTER at wedge time pins it
// inside sub_8256CBC0 itself (allocator CS sub_8256CA60 = prime suspect).
extern "C" void __imp__sub_822BD498(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_822BD498)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool log = gcWatchOn() && (n <= 20 || (n % 100000) == 0);
    if (log)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DGA] backing ENTER #%u size=%u align=%u pool=%08X lr=%08X",
            n, ctx.r3.u32, ctx.r4.u32, rd32(base, 0x8296F23C), (uint32_t)ctx.lr);
    __imp__sub_822BD498(ctx, base);
    if (log)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DGA] backing EXIT #%u -> %08X", n, ctx.r3.u32);
}

namespace {
inline void logFactory(const char* name, uint32_t rc, uint32_t n)
{
    if (n <= 8 || (n % 500000) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DGA] factory %s #%u -> %08X", name, n, rc);
}
} // namespace

PPC_FUNC(sub_8256A710)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    __imp__sub_8256A710(ctx, base);
    if (gcWatchOn()) logFactory("8256A710", ctx.r3.u32, n);
}
PPC_FUNC(sub_825EADE8)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    __imp__sub_825EADE8(ctx, base);
    if (gcWatchOn()) logFactory("825EADE8", ctx.r3.u32, n);
}
PPC_FUNC(sub_828A8F10)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    __imp__sub_828A8F10(ctx, base);
    if (gcWatchOn()) logFactory("828A8F10", ctx.r3.u32, n);
}

// ── iter-379b: DOGMA default-pool publish watch ─────────────────────────────
// sub_8278C7E8 is the 5-instruction setter that publishes the DEFAULT POOL into
// the fallback global 0x829A93F4 (called from FSGameApp_Init2_StartServices with
// dword_8296D890). The thread-registration path then copies that value into the
// per-thread pool slot. iter-379 proved our boot's slot holds 0x82966A10 (an
// arena-internal address, not a constructed pool) where golden holds 0x8296A3C8
// — this hook catches WHAT gets published and by WHOM. Pure observation.
extern "C" void __imp__sub_8278C7E8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8278C7E8)
{
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
        const uint32_t pool = ctx.r3.u32;
        uint32_t vt = 0;
        if (pool && sr::kernel::isGuestVAInRange(pool))
            vt = sr::kernel::readGuestU32BE(pool);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[DOGMA_DEFAULT_POOL] publish #%u r3=0x%08X (*r3=0x%08X, expect vt 0x82005E90) "
            "lr=0x%08X d890=0x%08X",
            n, pool, vt, (uint32_t)ctx.lr,
            sr::kernel::readGuestU32BE(0x8296D890u));
    }
    __imp__sub_8278C7E8(ctx, base);
}

// ── iter-379c: DOGMA thread-registration integrity watch ────────────────────
// sub_8278C700 (RegisterCurrentThread) writes entry KEY (line 18) then calls
// sub_825F4D50 (line 20) then writes the pool SLOT (line 22). The Dr0 watch
// proved a state where the KEY exists but the SLOT was never written - i.e. an
// invocation that entered and never completed. Entry/exit pairing names it.
extern "C" void __imp__sub_8278C700(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8278C700)
{
    static std::atomic<uint32_t> s_in{0}, s_out{0};
    const uint32_t i = s_in.fetch_add(1, std::memory_order_relaxed) + 1;
    if (i <= 10)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[C700] ENTER #%u lr=0x%08X r3=0x%08X", i, (uint32_t)ctx.lr, ctx.r3.u32);
    __imp__sub_8278C700(ctx, base);
    const uint32_t o = s_out.fetch_add(1, std::memory_order_relaxed) + 1;
    if (o <= 10)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[C700] EXIT  #%u (in=%u)", o, s_in.load(std::memory_order_relaxed));
}

// ── iter-380: NULL-item enqueue watch ───────────────────────────────────────
// ResSchedulerEnqueueSorted_sub_826F4830(sched, item) inserts the item pointer
// into queue A (+60) or B (+72) sorted by item->vt[116] priority. The Dr0 hunt
// proved the SR_DOGMA_ALLOC_SUB=0 init death is a NULL item enqueued here
// (guestPC=826F4A00, stored value 0): on console a NULL item would crash at the
// vtable deref, but our PPC_CALL_INDIRECT out-of-range fallback returns 0 and
// execution continues, so the NULL lands in the queue and the drain spins on it
// forever. This names the enqueue call site (lr) so the failed item alloc can
// be walked back. Pure observation.
extern "C" void __imp__sub_826F4830(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826F4830)
{
    if (!ctx.r4.u32) {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NULL_ENQUEUE] #%u sub_826F4830 item=NULL sched=0x%08X lr=0x%08X tid=%u",
                n, ctx.r3.u32, (uint32_t)ctx.lr, sr::kernel::getCurrentThreadHandle());
    } else {
        // iter-385f [ENQ]: log EVERY enqueue. The shallow wedge is the paced
        // preload's ENQUEUER stopping (queue holds only 2-5 tasks at a time;
        // post-wedge the queues are empty, no slot pinned, no PassivePoll —
        // the conveyor simply stops feeding). The last [ENQ] before the wedge
        // + its lr names the conveyor call site that died. ~1k lines/run.
        static std::atomic<uint32_t> s_a{0};
        const uint32_t a = s_a.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t item = ctx.r4.u32;
        char nm[40] = {0};
        const uint32_t np = sr::kernel::readGuestU32BE(item + 0x18);
        if (np >= 0x10000u && np < 0xC0000000u) {
            for (int i = 0; i < 39; ++i) {
                uint8_t* c = sr::kernel::guestVAToHost(np + i);
                if (!c || !*c || *c < 32 || *c >= 127) break;
                nm[i] = (char)*c;
            }
        }
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[ENQ] #%u sched=%08X item=%08X name='%s' tid=%u lr=%08X",
            a, ctx.r3.u32, item, nm,
            sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
    }
    __imp__sub_826F4830(ctx, base);
}

// ── iter-380: named-alloc dispatcher watch (sub_8278CAD8) ───────────────────
// The SECOND DOGMA dispatcher: vt[+8] of the static memory-interface singleton
// 0x82944A9C (golden vtable 0x8209A3D0, pool names "main memory/disposable/
// physical"). Same registry lookup as sub_8278D258 but dispatches
// pool->vt[+36](size, tag, flags, 0, 0, ctx, 0) with NO null/validity check on
// the resolved array[idx] pool. The SR_DOGMA_ALLOC_SUB=0 init death is this
// dispatcher returning 0 for the 80-byte "ResourceTask" alloc (ResMgrAcquire
// sub_826FAAC0 then enqueues the unconstructed NULL task). SR_DOGMA_ALLOC_SUB
// does NOT gate this function, so the ON/OFF divergence is pool STATE, not
// path. This logs the resolution chain (entry, idx, array, pool, vt) for the
// first calls and every failure. Pure observation.
static thread_local uint32_t t_lastC840Entry = 0;
extern "C" void __imp__sub_8278C840(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8278C840)
{
    __imp__sub_8278C840(ctx, base);
    t_lastC840Entry = ctx.r3.u32;
}

extern "C" void __imp__sub_8278CAD8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8278CAD8)
{
    const uint32_t size = ctx.r4.u32, tagVA = ctx.r5.u32, flags = ctx.r6.u32;
    const uint32_t lr = (uint32_t)ctx.lr;
    __imp__sub_8278CAD8(ctx, base);
    static std::atomic<uint32_t> s_n{0}, s_f{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool fail = ctx.r3.u32 == 0;
    const bool logIt =
        n <= 12 ||
        (fail && s_f.fetch_add(1, std::memory_order_relaxed) < 12);
    if (!logIt) return;
    char tag[32] = {0};
    if (tagVA >= 0x10000u && tagVA < 0xC0000000u) {
        const uint8_t* p = base + tagVA;
        for (unsigned i = 0; i + 1 < sizeof(tag) && p[i]; ++i)
            tag[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
    }
    const uint32_t entry = t_lastC840Entry;
    uint32_t idx = 0, arr = 0, pool = 0, poolVt = 0;
    if (entry >= 0x10000u) {
        idx = sr::kernel::readGuestU32BE(entry + 12);
        arr = sr::kernel::readGuestU32BE(entry + 16);
        if (arr >= 0x10000u && idx < 64)
            pool = sr::kernel::readGuestU32BE(arr + 4u * idx);
    } else {
        pool = sr::kernel::readGuestU32BE(0x829A93F4u);
    }
    if (pool >= 0x10000u) poolVt = sr::kernel::readGuestU32BE(pool);
    SR_LOG_BOOT(fail ? sr::LogSeverity::Error : sr::LogSeverity::Info,
        "[CAD8] #%u size=%u tag='%s' flags=%u entry=%08X idx=%u arr=%08X "
        "pool=%08X vt=%08X -> res=%08X lr=%08X tid=%u",
        n, size, tag, flags, entry, idx, arr, pool, poolVt, ctx.r3.u32, lr,
        sr::kernel::getCurrentThreadHandle());
}

#endif  // Oracle-only resource/allocator observers

