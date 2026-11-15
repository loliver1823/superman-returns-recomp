#pragma once
#include "ppc_config.h"

#ifndef PPC_CONTEXT_H_INCLUDED
#define PPC_CONTEXT_H_INCLUDED

#ifndef PPC_CONFIG_H_INCLUDED
#error "ppc_config.h must be included before ppc_context.h"
#endif

#include "kernel/guest_runtime.hpp"

#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <x86/avx.h>
#include <x86/sse.h>
#include <x86/sse4.1.h>

struct PPCContext;  // Forward declaration for macros and ppc_trap

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

#define PPC_JOIN(x, y) x##y
#define PPC_XSTRINGIFY(x) #x
#define PPC_STRINGIFY(x) PPC_XSTRINGIFY(x)
#define PPC_FUNC(x) void x(PPCContext& __restrict ctx, uint8_t* base)
#define PPC_FUNC_IMPL(x) extern "C" PPC_FUNC(x)
#define PPC_EXTERN_FUNC(x) extern PPC_FUNC(x)
#define PPC_EXTERN_IMPORT(x) extern "C" PPC_FUNC(x)
#define PPC_WEAK_FUNC(x) __attribute__((weak,noinline)) PPC_FUNC(x)

// L9-B (iter-216): full per-function-entry guest-execution trace. The `if (g_guestTraceOn)` guard
// is a single predicted-not-taken bool load -> ~free when SR_GUEST_TRACE is off; when on, emits one
// XENTRC01 record per function entry (VA parsed from __func__ = "__imp__sub_XXXXXXXX", r3/r1/lr from
// ctx). Matches Xenia's JIT-prolog granularity so the L9-C bisector frontier is exact, not a subset.
// iter-465 perf2: the prologue also refreshes `base` from the thread's guest base ONCE per
// entry and names it sr_gb, which PPC_GUEST_BASE() uses below - one register for every
// access of the function instead of a TLS read per access. Outside a generated function
// sr_gb names the global SrGuestBaseTlsRef object (a TLS read), so other users of the
// macros keep the old meaning. `base` (passed to callees, used by lvx/lwarx sites) gets
// the same value, so it can no longer differ from what the loads/stores use.
#define PPC_FUNC_PROLOGUE() \
    base = sr::kernel::g_guestBaseTls; \
    [[maybe_unused]] uint8_t* const sr_gb = base; \
    do { \
        __builtin_assume(((size_t)base & 0x1F) == 0); \
        if (__builtin_expect(sr::kernel::g_guestTraceOn, 0)) \
            sr::kernel::emitGuestTraceEntry(__func__, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r1.u32); \
    } while (0)

// Zero-extend 32-bit guest address to 64-bit host offset, with VA aliasing.
// Xbox 360 uncached (0xC0-0xDF) and write-combined (0xE0-0xFF) VAs alias to
// cached memory (0x80-0x9F). Our flat guest alloc only covers cached VAs, so
// redirect accesses through aliasGuestVA to keep loads/stores in range.
#ifndef PPC_GUEST_OFFSET
#define PPC_GUEST_OFFSET(x) ((uintptr_t)sr::kernel::aliasGuestVA((uint32_t)(x)))
#endif

// ReXGlue-generated code uses base (second parameter) and PPC_RAW_ADDR for SIMD loads/stores.
// Apply VA aliasing so uncached/WC accesses hit the same memory as cached.
#define PPC_PHYS_HOST_OFFSET(addr) 0u
#ifndef PPC_RAW_ADDR
#define PPC_RAW_ADDR(x) (PPC_GUEST_BASE() + (uintptr_t)sr::kernel::aliasGuestVA((uint32_t)(x)))
#endif

// ReXGlue-generated code may call ppc_trap for trap-word/trap-double and debug; stub for our harness.
#ifndef PPC_TRAP_DEFINED
#define PPC_TRAP_DEFINED
inline void ppc_trap(PPCContext& /*ctx*/, uint8_t* /*base*/, uint16_t trap_type) {
  (void)trap_type;
  /* Stub: could log and abort for trap_type 0/22, no-op for 25. */
}
#endif

// Use global guest base for all memory access. The 'base' parameter can be clobbered in the call
// chain (e.g. by tail calls or register reuse), leading to stores to host 0x7FFx00000000 and AV.
// The base is set by the harness before guest runs and is never overwritten by guest code.
// iter-461 perf: the thread-local itself, read inline (it was an out-of-line getter call on
// EVERY guest load, store and SIMD access - ~600,000 sites; the compiler hoists the TLS
// address per function now).
struct SrGuestBaseTlsRef {
    operator uint8_t*() const noexcept { return sr::kernel::g_guestBaseTls; }
};
inline constexpr SrGuestBaseTlsRef sr_gb{};
#define PPC_GUEST_BASE() ((uint8_t*)sr_gb)

#ifndef PPC_LOAD_U8
#define PPC_LOAD_U8(x) *(volatile uint8_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(x))
#endif

#ifndef PPC_LOAD_U16
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_U32
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(x)))
#endif

#ifndef PPC_LOAD_U64
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(x)))
#endif

// TODO: Implement.
// These are currently unused. However, MMIO loads could possibly be handled statically with some profiling and a fallback.
// The fallback would be a runtime exception handler which will intercept reads from MMIO regions 
// and log the PC for compiling to static code later.
#ifndef PPC_MM_LOAD_U8
#define PPC_MM_LOAD_U8(x)  PPC_LOAD_U8 (x)
#endif

#ifndef PPC_MM_LOAD_U16
#define PPC_MM_LOAD_U16(x) PPC_LOAD_U16(x)
#endif

#ifndef PPC_MM_LOAD_U32
#define PPC_MM_LOAD_U32(x) PPC_LOAD_U32(x)
#endif

#ifndef PPC_MM_LOAD_U64
#define PPC_MM_LOAD_U64(x) PPC_LOAD_U64(x)
#endif

// iter-465 perf2: the PPC_STORE debug watchpoints (module globals, DOGMA guard, vtable area,
// the dynamic render-object / FMUI-queue / Apt-env / world-pointer watches) are COMPILED OUT
// unless the build defines SR_STORE_WATCHPOINTS=1 (CMake option SR_STORE_WATCHPOINTS, default
// OFF). Every watch callback only logs (no platform fix hides in them), but the runtime gate
// was armed in EVERY game: the sub_826F92E0 observer sets g_storeWatchOn at boot, so each
// 32/64-bit guest store paid the constant-range compares plus three atomic loads. With the
// option ON the macros are exactly the iter-461 ones (SR_STORE_WATCH=1 / kernel arm sites).
#ifndef SR_STORE_WATCHPOINTS
#define SR_STORE_WATCHPOINTS 0
#endif
#define SR_STORE_WATCH_ARMED() (SR_STORE_WATCHPOINTS && __builtin_expect(sr::kernel::g_storeWatchOn, 0))

#ifndef PPC_STORE_U8
// iter-24x: watch byte stores into modA-D range to catch sub-word zeroing.
#define PPC_STORE_U8(x, y) do { \
    uint32_t _sb_a = (uint32_t)(x); \
    if (SR_STORE_WATCH_ARMED()) /* iter-461: SR_STORE_WATCH; iter-465 perf2: compiled out unless SR_STORE_WATCHPOINTS */ \
    if (__builtin_expect((_sb_a >= 0x8296D8D8u && _sb_a <= 0x8296D8E7u) \
        /* iter-191: Apt flow singleton @829A23E0 / env @829A23E4 — catch the scribbler poisoning them */ \
        || (_sb_a >= 0x829A23E0u && _sb_a <= 0x829A23E7u) \
        /* iter-296g: importCb slot zeroer hunt */ \
        || (_sb_a >= 0x829A1AC0u && _sb_a <= 0x829A1AC3u), 0)) { \
        sr::kernel::moduleGlobalWatchpoint(_sb_a, (uint8_t)(y), __FILE__, __LINE__); \
    } \
    *(volatile uint8_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(_sb_a)) = (y); \
} while(0)
#endif

#ifndef PPC_STORE_U16
// iter-24x: watch halfword stores into modA-D range.
#define PPC_STORE_U16(x, y) do { \
    uint32_t _sh_a = (uint32_t)(x); \
    if (SR_STORE_WATCH_ARMED()) /* iter-461: SR_STORE_WATCH; iter-465 perf2: compiled out unless SR_STORE_WATCHPOINTS */ \
    if (__builtin_expect((_sh_a >= 0x8296D8D8u && _sh_a <= 0x8296D8E6u) \
        /* iter-191: Apt flow singleton/env window (overlap check) */ \
        || (_sh_a + 1u >= 0x829A23E0u && _sh_a <= 0x829A23E7u), 0)) { \
        sr::kernel::moduleGlobalWatchpoint(_sh_a, (uint16_t)(y), __FILE__, __LINE__); \
    } \
    *(volatile uint16_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(_sh_a)) = __builtin_bswap16(y); \
} while(0)
#endif

#ifndef PPC_STORE_U32
// Watchpoints: DOGMA guard1 (0x829AAE78), module globals, vtable area (0xD0060000-0xD00603FF).
// Dispatch-table watchpoint was removed 2026-04-24 when the dispatch table moved to a host-only
// allocation; the former VA range [0x829E0000..0x83C00000) is now plain zeroed guest RAM.
#define PPC_STORE_U32(x, y) do { \
    uint32_t _sw_a = (uint32_t)(x); \
    uint32_t _sw_v = (y); \
    if (SR_STORE_WATCH_ARMED()) /* iter-461: SR_STORE_WATCH; iter-465 perf2: compiled out unless SR_STORE_WATCHPOINTS */ \
    if (__builtin_expect(_sw_a == 0x829AAE78u, 0)) { \
        sr::kernel::dogmaGuardWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
    } else if (__builtin_expect((_sw_a + 3u >= 0x8296D8D8u && _sw_a <= 0x8296D8E7u) \
        /* iter-189: range overlap — misaligned stride-8 zero stores at ...D7/DF/E7 evaded the == checks */ \
        || _sw_a == 0x829AD878u || _sw_a == 0x829AD87Cu || _sw_a == 0x829AD880u || _sw_a == 0x829AD884u \
        || _sw_a == 0x829A6880u || _sw_a == 0x829A9978u \
        /* iter-191: Apt flow singleton @829A23E0 / env @829A23E4 (overlap check, scribbler hunt) */ \
        || (_sw_a + 3u >= 0x829A23E0u && _sw_a <= 0x829A23E7u) \
        /* iter-193: g3/EB80 — who zeroes the flow client right after the AUI drive? */ \
        || (_sw_a + 3u >= 0x8296EB80u && _sw_a <= 0x8296EB83u) \
        /* iter-296g: importCb slot 0x829A1AC0 — who RE-ZEROES it between register attempts? */ \
        || (_sw_a + 3u >= 0x829A1AC0u && _sw_a <= 0x829A1AC3u), 0)) { \
        sr::kernel::moduleGlobalWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
    } else if (__builtin_expect(_sw_a >= 0x829F0000u && _sw_a < 0x829F0400u, 0)) { \
        sr::kernel::vtableWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
    } else { \
        uint32_t _ro_va = sr::kernel::g_renderObjVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_ro_va != 0 && _sw_a >= _ro_va && _sw_a < _ro_va + 840u, 0)) { \
            sr::kernel::renderObjDestructWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
        } \
        /* iter-191: FMUI pool queue {begin,end} fields (workerVA+180..187) */ \
        uint32_t _fq_va = sr::kernel::g_fmuiPoolQueueVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_fq_va != 0 && _sw_a + 3u >= _fq_va && _sw_a < _fq_va + 8u, 0)) { \
            sr::kernel::fmuiQueueWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
        } \
        /* iter-192: Apt env object [env, env+16) — vtable-zeroer hunt */ \
        uint32_t _ae_va = sr::kernel::g_aptEnvVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_ae_va != 0 && _sw_a + 3u >= _ae_va && _sw_a < _ae_va + 16u, 0)) { \
            sr::kernel::fmuiQueueWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
        } \
        /* iter-292e: FM hash-set base ptr (module+4) — pin the corrupting writer */ \
        if (__builtin_expect(sr::kernel::g_worldPtrWatchArmed.load(std::memory_order_relaxed), 0)) { \
            for (int _wp = 0; _wp < 4; ++_wp) { \
                uint32_t _wp_va = sr::kernel::g_worldPtrFieldVA[_wp].load(std::memory_order_relaxed); \
                if (_wp_va != 0 && _sw_a + 3u >= _wp_va && _sw_a <= _wp_va + 3u) \
                    sr::kernel::worldPtrFieldWatchpoint(_sw_a, _sw_v, __FILE__, __LINE__); \
            } \
        } \
    } \
    *(volatile uint32_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(_sw_a)) = __builtin_bswap32(_sw_v); \
} while(0)
#endif

#ifndef PPC_STORE_U64
// iter-24x: watch 64-bit stores into modA-D range. A U64 store at 0x8296D8D8
// straddles modD/modB; one at 0x8296D8E0 straddles modC/modA. Either zeroes
// two of the four globals at once.
#define PPC_STORE_U64(x, y) do { \
    uint32_t _sw64_a = (uint32_t)(x); \
    uint64_t _sw64_v = (y); \
    if (SR_STORE_WATCH_ARMED()) /* iter-461: SR_STORE_WATCH; iter-465 perf2: compiled out unless SR_STORE_WATCHPOINTS */ \
    if (__builtin_expect(_sw64_a >= 0x829EFFFCu && _sw64_a < 0x829F0400u, 0)) { \
        sr::kernel::vtableWatchpoint(_sw64_a, (uint32_t)(_sw64_v >> 32), __FILE__, __LINE__); \
    } else if (__builtin_expect((_sw64_a >= 0x8296D8D0u && _sw64_a <= 0x8296D8E4u) \
        /* iter-191: Apt flow singleton/env window (straddle check) */ \
        || (_sw64_a + 7u >= 0x829A23E0u && _sw64_a <= 0x829A23E7u) \
        /* iter-296g: importCb slot zeroer hunt (straddle check) */ \
        || (_sw64_a + 7u >= 0x829A1AC0u && _sw64_a <= 0x829A1AC3u), 0)) { \
        sr::kernel::moduleGlobalWatchpoint(_sw64_a, (uint32_t)(_sw64_v >> 32), __FILE__, __LINE__); \
        sr::kernel::moduleGlobalWatchpoint(_sw64_a + 4, (uint32_t)(_sw64_v & 0xFFFFFFFFu), __FILE__, __LINE__); \
    } else if (__builtin_expect(_sw64_a == 0x829A9978u || _sw64_a == 0x829A9974u, 0)) { \
        sr::kernel::moduleGlobalWatchpoint(_sw64_a, (uint32_t)((_sw64_v >> 32) & 0xFFFFFFFFu), __FILE__, __LINE__); \
        sr::kernel::moduleGlobalWatchpoint(_sw64_a + 4, (uint32_t)(_sw64_v & 0xFFFFFFFFu), __FILE__, __LINE__); \
    } else { \
        uint32_t _ro_va64 = sr::kernel::g_renderObjVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_ro_va64 != 0 && _sw64_a >= _ro_va64 && _sw64_a < _ro_va64 + 840u, 0)) { \
            sr::kernel::renderObjDestructWatchpoint(_sw64_a, (uint32_t)(_sw64_v >> 32), __FILE__, __LINE__); \
        } \
        /* iter-191: FMUI pool queue {begin,end} fields (workerVA+180..187) */ \
        uint32_t _fq_va64 = sr::kernel::g_fmuiPoolQueueVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_fq_va64 != 0 && _sw64_a + 7u >= _fq_va64 && _sw64_a < _fq_va64 + 8u, 0)) { \
            sr::kernel::fmuiQueueWatchpoint(_sw64_a, (uint32_t)(_sw64_v >> 32), __FILE__, __LINE__); \
            sr::kernel::fmuiQueueWatchpoint(_sw64_a + 4, (uint32_t)(_sw64_v & 0xFFFFFFFFu), __FILE__, __LINE__); \
        } \
        /* iter-192: Apt env object [env, env+16) — vtable-zeroer hunt */ \
        uint32_t _ae_va64 = sr::kernel::g_aptEnvVA.load(std::memory_order_relaxed); \
        if (__builtin_expect(_ae_va64 != 0 && _sw64_a + 7u >= _ae_va64 && _sw64_a < _ae_va64 + 16u, 0)) { \
            sr::kernel::fmuiQueueWatchpoint(_sw64_a, (uint32_t)(_sw64_v >> 32), __FILE__, __LINE__); \
            sr::kernel::fmuiQueueWatchpoint(_sw64_a + 4, (uint32_t)(_sw64_v & 0xFFFFFFFFu), __FILE__, __LINE__); \
        } \
        /* iter-292e: FM hash-set base ptr (module+4) — pin the corrupting writer (64-bit/straddling store) */ \
        if (__builtin_expect(sr::kernel::g_worldPtrWatchArmed.load(std::memory_order_relaxed), 0)) { \
            for (int _wp = 0; _wp < 4; ++_wp) { \
                uint32_t _wp_va = sr::kernel::g_worldPtrFieldVA[_wp].load(std::memory_order_relaxed); \
                if (_wp_va != 0 && _sw64_a + 7u >= _wp_va && _sw64_a <= _wp_va + 3u) { \
                    uint32_t _wp_val = (_wp_va >= _sw64_a + 4u) ? (uint32_t)(_sw64_v & 0xFFFFFFFFu) : (uint32_t)(_sw64_v >> 32); \
                    sr::kernel::worldPtrFieldWatchpoint(_wp_va, _wp_val, __FILE__, __LINE__); \
                } \
            } \
        } \
    } \
    *(volatile uint64_t*)(PPC_GUEST_BASE() + PPC_GUEST_OFFSET(_sw64_a)) = __builtin_bswap64(_sw64_v); \
} while(0)
#endif

// MMIO Store handling is completely reliant on being preeceded by eieio.
// TODO: Verify if that's always the case.
#ifndef PPC_MM_STORE_U8
#define PPC_MM_STORE_U8(x, y)   PPC_STORE_U8 (x, y)
#endif

#ifndef PPC_MM_STORE_U16
#define PPC_MM_STORE_U16(x, y)  PPC_STORE_U16(x, y)
#endif

#ifndef PPC_MM_STORE_U32
#define PPC_MM_STORE_U32(x, y)  PPC_STORE_U32(x, y)
#endif

#ifndef PPC_MM_STORE_U64
#define PPC_MM_STORE_U64(x, y)  PPC_STORE_U64(x, y)
#endif

#ifndef PPC_CALL_FUNC
#define PPC_CALL_FUNC(x) x(ctx, base)
#endif

#define PPC_MEMORY_SIZE 0x100000000ull

// Dispatch table lives in a host-only allocation (see guest_harness.cpp). Single deref
// into sr::kernel::g_dispatchTable — no guest-memory access, so pool allocators that
// land in the old table VA range [0x829E0000..0x83C00000) can't stomp function pointers.
#define PPC_LOOKUP_FUNC(x, y) (sr::kernel::g_dispatchTable[(uint32_t(y) - PPC_CODE_BASE) >> 2])
// iter-189: range-checked lookup. A garbage VA (e.g. 0x8B479FEC from corrupted guest
// data) indexes megabytes past the table and AVs on the READ itself — before any
// null-check of the result (crash dump iter189e, audio render thread). Use this for
// any VA that comes from guest memory rather than the recompiler.
#define PPC_LOOKUP_FUNC_SAFE(x, y) \
    ((uint32_t(y) >= PPC_CODE_BASE && uint32_t(y) < PPC_CODE_BASE + PPC_CODE_SIZE) \
        ? PPC_LOOKUP_FUNC(x, y) : (PPCFunc*)nullptr)

#ifndef PPC_CALL_INDIRECT_FUNC
namespace sr { namespace kernel {
// iter-463 charter (burndown item 10, kernel/indirect_call.cpp): an indirect
// call whose target resolves to no recompiled function - NULL, outside the
// code range, or an in-range VA with no dispatch entry. Default exe: logged
// ([ICALL_UNRESOLVED]: call site, target, the object in r3 and its vtable)
// then a NAMED FAIL ([ICALL_FAIL]); SR_ALLOW_FORCING=1 keeps the legacy r3=0.
void indirectCallUnresolved(struct PPCContext& ctx, uint32_t target);
}}
// Every guest indirect call (bctrl/bcctrl) dispatches through the host-side
// table; the low two bits of the target are ignored as bcctr ignores them.
// iter-463: NO fabricated returns here any more. The deleted branches:
//  - NULL target -> r3 = 0;
//  - 0x822979C4 ("fake hash entry"), 0x8276A520 / 0x827648B4 ("mid-function
//    callbacks") -> r3 = 0: return addresses with 0 references in all three
//    golden dumps; nothing legitimate calls them;
//  - 0x827B0400 / 0x827A7B28 -> r3 = 0: a real CRT C initializer (startup
//    table 0x82910E7C: _setmbcp(-3) + mbc info, once-guard 0x829B2FB4 = 1 and
//    0x829AB0D0 = -3 in all three golden dumps) and a real CRT terminator
//    (0x82910E8C) - both recompiled and mapped; the old "dispatch returns a
//    stub" bug they papered over died with the guest-memory dispatch table.
//    They now RUN;
//  - in-range unmapped and out-of-range targets -> r3 = 0.
#define PPC_CALL_INDIRECT_FUNC(x) do { \
    uint32_t _target = (x); \
    if (__builtin_expect(sr::kernel::g_traceCallsOn, 0)) /* iter-461: the call itself was the cost */ \
        sr::kernel::traceIndirectCall(_target, (uint32_t)ctx.lr); /* iter-214 L4: SR_TRACE_CALLS */ \
    PPCFunc* _fn = (_target >= PPC_CODE_BASE && _target < (PPC_CODE_BASE + PPC_CODE_SIZE)) \
        ? PPC_LOOKUP_FUNC(base, _target) : (PPCFunc*)nullptr; \
    if (__builtin_expect(_fn != nullptr, 1)) { \
        _fn(ctx, PPC_GUEST_BASE()); \
    } else { \
        sr::kernel::indirectCallUnresolved(ctx, _target); \
    } \
} while(0)
#endif

typedef void PPCFunc(struct PPCContext& __restrict__ ctx, uint8_t* base);

struct PPCFuncMapping
{
    size_t guest;
    PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];

namespace sr { namespace kernel {
// Host-side dispatch table. Indexed by (guestVA - PPC_CODE_BASE)/4. Populated once at
// boot by the guest harness; read-only thereafter. Single pointer-deref in PPC_LOOKUP_FUNC.
extern PPCFunc** g_dispatchTable;
constexpr size_t kDispatchTableEntryCount = (0x82A00000u - PPC_CODE_BASE) / 4;
}}  // namespace sr::kernel

union PPCRegister
{
    int8_t s8;
    uint8_t u8;
    int16_t s16;
    uint16_t u16;
    int32_t s32;
    uint32_t u32;
    int64_t s64;
    uint64_t u64;
    float f32;
    double f64;
};

struct PPCXERRegister
{
    uint8_t so;
    uint8_t ov;
    uint8_t ca;
};

struct PPCCRRegister
{
    uint8_t lt;
    uint8_t gt;
    uint8_t eq;
    union
    {
        uint8_t so;
        uint8_t un;
    };

    template<typename T>
    inline void compare(T left, T right, const PPCXERRegister& xer) noexcept
    {
        lt = left < right;
        gt = left > right;
        eq = left == right;
        so = xer.so;
    }

    inline void compare(double left, double right) noexcept
    {
        un = __builtin_isnan(left) || __builtin_isnan(right);
        lt = !un && (left < right);
        gt = !un && (left > right);
        eq = !un && (left == right);
    }

    inline void setFromMask(simde__m128 mask, int imm) noexcept
    {
        int m = simde_mm_movemask_ps(mask);
        lt = m == imm; // all equal
        gt = 0;
        eq = m == 0; // none equal
        so = 0;
    }

    inline void setFromMask(simde__m128i mask, int imm) noexcept
    {
        int m = simde_mm_movemask_epi8(mask);
        lt = m == imm; // all equal
        gt = 0;
        eq = m == 0; // none equal
        so = 0;
    }
};

union alignas(0x10) PPCVRegister
{
    int8_t s8[16];
    uint8_t u8[16];
    int16_t s16[8];
    uint16_t u16[8];
    int32_t s32[4];
    uint32_t u32[4];
    int64_t s64[2];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
};

#define PPC_ROUND_NEAREST 0x00
#define PPC_ROUND_TOWARD_ZERO 0x01
#define PPC_ROUND_UP 0x02
#define PPC_ROUND_DOWN 0x03
#define PPC_ROUND_MASK 0x03

struct PPCFPSCRRegister
{
    uint32_t csr;

    static constexpr size_t HostToGuest[] = { PPC_ROUND_NEAREST, PPC_ROUND_DOWN, PPC_ROUND_UP, PPC_ROUND_TOWARD_ZERO };

    // simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)
    static constexpr size_t RoundShift = 13;
    static constexpr size_t RoundMask = SIMDE_MM_ROUND_MASK;
    static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
    static constexpr size_t GuestToHost[] = { SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO, SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN };

    inline uint32_t getcsr() noexcept
    {
        return simde_mm_getcsr();
    }

    inline void setcsr(uint32_t csr) noexcept
    {
        // CRITICAL: Always force exception mask bits to be set (bits 7-12)
        // This prevents FPU exceptions that would crash the recompiled code.
        // Xbox 360 PowerPC doesn't raise these exceptions, so we must suppress them.
        // Fix applied: 2026-02-04
        csr |= 0x1F80;  // Set all exception mask bits
        simde_mm_setcsr(csr);
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    // RMode
    static constexpr size_t RoundShift = 22;
    static constexpr size_t RoundMask = 3 << RoundShift;
    // FZ and FZ16
    static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
    // Nearest, Zero, -Infinity, -Infinity
    static constexpr size_t GuestToHost[] = { 0 << RoundShift, 3 << RoundShift, 1 << RoundShift, 2 << RoundShift };

    inline uint32_t getcsr() noexcept
    {
        uint64_t csr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
        return csr;
    }

    inline void setcsr(uint32_t csr) noexcept
    {
        __asm__ __volatile__("msr fpcr, %0" : : "r"(csr));
    }
#else
#   error "Missing implementation for FPSCR."
#endif

    inline uint32_t loadFromHost() noexcept
    {
        csr = getcsr();
        return HostToGuest[(csr & RoundMask) >> RoundShift];
    }
        
    inline void storeFromGuest(uint32_t value) noexcept
    {
        csr &= ~RoundMask;
        csr |= GuestToHost[value & PPC_ROUND_MASK];
        setcsr(csr);
    }

    inline void enableFlushModeUnconditional() noexcept
    {
        csr |= FlushMask;
        setcsr(csr);
    }

    inline void disableFlushModeUnconditional() noexcept
    {
        csr &= ~FlushMask;
        setcsr(csr);
    }

    inline void enableFlushMode() noexcept
    {
        if ((csr & FlushMask) != FlushMask) [[unlikely]]
        {
            csr |= FlushMask;
            setcsr(csr);
        }
    }

    inline void disableFlushMode() noexcept
    {
        if ((csr & FlushMask) != 0) [[unlikely]]
        {
            csr &= ~FlushMask;
            setcsr(csr);
        }
    }
};

struct alignas(0x40) PPCContext
{
    PPCRegister r3;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    PPCRegister r0;
#endif
    PPCRegister r1;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    PPCRegister r2;
#endif
    PPCRegister r4;
    PPCRegister r5;
    PPCRegister r6;
    PPCRegister r7;
    PPCRegister r8;
    PPCRegister r9;
    PPCRegister r10;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    PPCRegister r11;
    PPCRegister r12;
#endif
    PPCRegister r13;
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    PPCRegister r14;
    PPCRegister r15;
    PPCRegister r16;
    PPCRegister r17;
    PPCRegister r18;
    PPCRegister r19;
    PPCRegister r20;
    PPCRegister r21;
    PPCRegister r22;
    PPCRegister r23;
    PPCRegister r24;
    PPCRegister r25;
    PPCRegister r26;
    PPCRegister r27;
    PPCRegister r28;
    PPCRegister r29;
    PPCRegister r30;
    PPCRegister r31;
#endif

#ifndef PPC_CONFIG_SKIP_LR
    uint64_t lr;
#endif
#ifndef PPC_CONFIG_CTR_AS_LOCAL
    PPCRegister ctr;
#endif
#ifndef PPC_CONFIG_XER_AS_LOCAL
    PPCXERRegister xer;
#endif
#ifndef PPC_CONFIG_RESERVED_AS_LOCAL
    PPCRegister reserved;
#endif
#ifndef PPC_CONFIG_SKIP_MSR
    uint32_t msr = 0x200A000;
#endif
#ifndef PPC_CONFIG_CR_AS_LOCAL
    PPCCRRegister cr0;
    PPCCRRegister cr1;
    PPCCRRegister cr2;
    PPCCRRegister cr3;
    PPCCRRegister cr4;
    PPCCRRegister cr5;
    PPCCRRegister cr6;
    PPCCRRegister cr7;
#endif
    PPCFPSCRRegister fpscr;

#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    PPCRegister f0;
#endif
    PPCRegister f1;
    PPCRegister f2;
    PPCRegister f3;
    PPCRegister f4;
    PPCRegister f5;
    PPCRegister f6;
    PPCRegister f7;
    PPCRegister f8;
    PPCRegister f9;
    PPCRegister f10;
    PPCRegister f11;
    PPCRegister f12;
    PPCRegister f13;
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    PPCRegister f14;
    PPCRegister f15;
    PPCRegister f16;
    PPCRegister f17;
    PPCRegister f18;
    PPCRegister f19;
    PPCRegister f20;
    PPCRegister f21;
    PPCRegister f22;
    PPCRegister f23;
    PPCRegister f24;
    PPCRegister f25;
    PPCRegister f26;
    PPCRegister f27;
    PPCRegister f28;
    PPCRegister f29;
    PPCRegister f30;
    PPCRegister f31;
#endif

    PPCVRegister v0;
    PPCVRegister v1;
    PPCVRegister v2;
    PPCVRegister v3;
    PPCVRegister v4;
    PPCVRegister v5;
    PPCVRegister v6;
    PPCVRegister v7;
    PPCVRegister v8;
    PPCVRegister v9;
    PPCVRegister v10;
    PPCVRegister v11;
    PPCVRegister v12;
    PPCVRegister v13;
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    PPCVRegister v14;
    PPCVRegister v15;
    PPCVRegister v16;
    PPCVRegister v17;
    PPCVRegister v18;
    PPCVRegister v19;
    PPCVRegister v20;
    PPCVRegister v21;
    PPCVRegister v22;
    PPCVRegister v23;
    PPCVRegister v24;
    PPCVRegister v25;
    PPCVRegister v26;
    PPCVRegister v27;
    PPCVRegister v28;
    PPCVRegister v29;
    PPCVRegister v30;
    PPCVRegister v31;
#endif
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    PPCVRegister v32;
    PPCVRegister v33;
    PPCVRegister v34;
    PPCVRegister v35;
    PPCVRegister v36;
    PPCVRegister v37;
    PPCVRegister v38;
    PPCVRegister v39;
    PPCVRegister v40;
    PPCVRegister v41;
    PPCVRegister v42;
    PPCVRegister v43;
    PPCVRegister v44;
    PPCVRegister v45;
    PPCVRegister v46;
    PPCVRegister v47;
    PPCVRegister v48;
    PPCVRegister v49;
    PPCVRegister v50;
    PPCVRegister v51;
    PPCVRegister v52;
    PPCVRegister v53;
    PPCVRegister v54;
    PPCVRegister v55;
    PPCVRegister v56;
    PPCVRegister v57;
    PPCVRegister v58;
    PPCVRegister v59;
    PPCVRegister v60;
    PPCVRegister v61;
    PPCVRegister v62;
    PPCVRegister v63;
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    PPCVRegister v64;
    PPCVRegister v65;
    PPCVRegister v66;
    PPCVRegister v67;
    PPCVRegister v68;
    PPCVRegister v69;
    PPCVRegister v70;
    PPCVRegister v71;
    PPCVRegister v72;
    PPCVRegister v73;
    PPCVRegister v74;
    PPCVRegister v75;
    PPCVRegister v76;
    PPCVRegister v77;
    PPCVRegister v78;
    PPCVRegister v79;
    PPCVRegister v80;
    PPCVRegister v81;
    PPCVRegister v82;
    PPCVRegister v83;
    PPCVRegister v84;
    PPCVRegister v85;
    PPCVRegister v86;
    PPCVRegister v87;
    PPCVRegister v88;
    PPCVRegister v89;
    PPCVRegister v90;
    PPCVRegister v91;
    PPCVRegister v92;
    PPCVRegister v93;
    PPCVRegister v94;
    PPCVRegister v95;
    PPCVRegister v96;
    PPCVRegister v97;
    PPCVRegister v98;
    PPCVRegister v99;
    PPCVRegister v100;
    PPCVRegister v101;
    PPCVRegister v102;
    PPCVRegister v103;
    PPCVRegister v104;
    PPCVRegister v105;
    PPCVRegister v106;
    PPCVRegister v107;
    PPCVRegister v108;
    PPCVRegister v109;
    PPCVRegister v110;
    PPCVRegister v111;
    PPCVRegister v112;
    PPCVRegister v113;
    PPCVRegister v114;
    PPCVRegister v115;
    PPCVRegister v116;
    PPCVRegister v117;
    PPCVRegister v118;
    PPCVRegister v119;
    PPCVRegister v120;
    PPCVRegister v121;
    PPCVRegister v122;
    PPCVRegister v123;
    PPCVRegister v124;
    PPCVRegister v125;
    PPCVRegister v126;
    PPCVRegister v127;
#endif
};

inline uint8_t VectorMaskL[] =
{
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] =
{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] =
{
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] =
{
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b)
{
    return simde_mm_add_epi32(a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b)
{
    simde__m128i c = simde_mm_set1_epi8(char(128));
    return simde_mm_xor_si128(c, simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b)
{
    simde__m128i c = simde_mm_set1_epi16(short(32768));
    return simde_mm_xor_si128(c, simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1)
{
    simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
    simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
    xmm0 = simde_mm_srli_epi32(xmm0, 31);
    xmm0 = simde_mm_add_epi32(xmm0, xmm1);
    xmm0 = simde_mm_srai_epi32(xmm0, 8);
    xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
    simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
    return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c)
{
    simde__m128i d = simde_mm_set1_epi8(0xF);
    simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
    return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e), simde_mm_slli_epi32(c, 3));
}

inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b)
{
    simde__m128i c = simde_mm_set1_epi8(char(128));
    return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b)
{
    simde__m128i c = simde_mm_set1_epi16(short(32768));
    return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

inline simde__m128i simde_mm_vctsxs(simde__m128 src1)
{
    simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
    simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
    simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
    xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
    simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)), simde_mm_castsi128_ps(xmm1));
    return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

inline simde__m128i simde_mm_vctuxs(simde__m128 src1)
{
    // float -> u32 with saturation: NaN/negative -> 0, >= 2^32 -> 0xFFFFFFFF
    simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps()); // max(NaN,0)=0
    simde__m128 two31 = simde_mm_set1_ps(2147483648.0f);
    simde__m128 big = simde_mm_cmpge_ps(clamped, two31);
    simde__m128i lo = simde_mm_cvttps_epi32(clamped);
    simde__m128i hi = simde_mm_add_epi32(
        simde_mm_cvttps_epi32(simde_mm_sub_ps(clamped, two31)),
        simde_mm_set1_epi32(INT_MIN));
    simde__m128i res = simde_mm_blendv_epi8(lo, hi, simde_mm_castps_si128(big));
    simde__m128 over = simde_mm_cmpge_ps(clamped, simde_mm_set1_ps(4294967296.0f));
    return simde_mm_or_si128(res, simde_mm_castps_si128(over));
}

inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b)
{
    b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
    return simde_mm_castps_si128(simde_mm_insert_ps(simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)), simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

#if defined(__aarch64__) || defined(_M_ARM64)
inline uint64_t __rdtsc()
{
    uint64_t ret;
    asm volatile("mrs %0, cntvct_el0\n\t"
                 : "=r"(ret)::"memory");
    return ret;
}
#elif !defined(__x86_64__) && !defined(_M_X64)
#   error "Missing implementation for __rdtsc()"
#endif

#endif
