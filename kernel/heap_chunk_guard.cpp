// iter-296h: heap-chunk guard — the TRUE boundary for the static-globals
// scribble class.
//
// sub_828CEBC0 = the guest heap allocator core (dlmalloc-shaped: small/large
// size bins, boundary tags, top chunk at heap[0x111], binmap heap[0x10d..],
// morecore vt[0xa8]; returns chunk+8). On bad rolls its free lists are
// POISONED with a bogus chunk inside the binary/statics window (some earlier
// broken free), so malloc returns pointers into 0x829Axxxx BSS — and then
// perfectly legitimate copy/fill code (VectorAssignFill, sub_826F6208,
// sub_826EEA68, sub_82784898 — all watchpoint-caught writing the importCb
// slot 0x829A1AC0 with lr=0x825F70E0) scribbles the Apt/AS globals, killing
// the register/menu chain. Guarding individual fill sites can't win; the
// allocator's RETURN VALUE is the choke point: no legitimate heap chunk can
// live inside [0x82000000, 0x82A00000) (binary image + statics).
//
// Strategy: call the real allocator; if the returned chunk lies in the
// forbidden window, DISCARD it (the allocator already unlinked it) and call
// again — each retry pops the next candidate, progressively cleansing the
// poisoned bin. Bounded at 8 discards per call. SR_HEAPCHUNK_GUARD=0 keeps
// the A/B. Reached only via the strategy pointer at 0x820B317C (no static
// callers) — the mapping table binds indirect dispatch to this strong def.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>

extern "C" void __imp__sub_828CEBC0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
// iter-463 charter (burndown U1): a registered band-aid (discard + retry the
// guest malloc core's result) that shipped default ON; "[HEAPCHUNK_GUARD]" is
// in 1/81 run logs before the heap-corruption fix 60a90bf and 0/69 after. It
// now needs SR_ALLOW_FORCING (=0 keeps the A/B under the master gate).
bool guardEnabled()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_HEAPCHUNK_GUARD");
        return (!e || e[0] != '0') && sr::kernel::forcingAllowed();
    }();
    return v;
}
// iter-296h refinement: a LEGIT small BSS arena lives at ~0x82966928..0x8296A000
// (one early-boot burst of 333 sequential carves, caller lr=0x828CEBA8) — the
// first window [0x82000000,0x82A00000) wrongly discarded its chunks and burned
// arena memory to exhaustion (final alloc=0). The scribble evidence (importCb
// 0x829A1AC0, intern buckets 0x829A17xx) is all in 0x829Axxxx: protect ONLY
// the Apt/AS statics page range.
constexpr uint32_t kForbidLo = 0x829A0000u;
constexpr uint32_t kForbidHi = 0x829C0000u;
} // namespace

// iter-376 H1 DIAGNOSTIC (SR_HEAP_MSTATE_LOG=1, default OFF, READ-ONLY).
//
// The FE service runner wedges permanently at lr=0x828CED60 — inside THIS
// function's release-list drain, which exits only when the chain rooted at
// mstate+60 reaches the sentinel mstate+52. A circular chain walks forever with
// no fault and no crash, which matches the observed silent freeze exactly.
//
// H1 says the chain is circular because the mstate is one WE fabricated:
// SR_HEAP_STUB (kernel/guest_context.cpp, own-gated default-ON, its own comment
// reads "This is UNGATED FORCING (fake mstate)") publishes 0x82110000, memsets
// 0x2000 to zero, and initialises only +1408 and a crit sect at +0x1000. So the
// bin heads stay 0 instead of self-circular. It also writes guest VA 8 =
// 0x00010000 as a guessed "top pointer" — but for a chunk at VA 0, +8 IS the
// forward pointer, giving 0 -> 0x10000 -> 0 -> ... forever.
//
// This logs the mstate the guest actually passes. If every call carries a real
// heap object, H1 is dead in one run — which is the point: it is built to lose.
// Reads nothing but r3 (already in registers), so it cannot fault.
namespace {
bool mstateLogEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("SR_HEAP_MSTATE_LOG");
        return e && e[0] == '1';
    }();
    return v;
}
} // namespace

PPC_FUNC(sub_828CEBC0)
{
    if (mstateLogEnabled()) {
        // Census by mstate value: the question is whether 0x82110000 (our fake)
        // or 0 (the "null heap" path the stub also provisions) ever appears.
        static std::atomic<uint64_t> s_calls{0}, s_fake{0}, s_null{0}, s_real{0};
        const uint32_t ms = ctx.r3.u32;
        if (ms == 0x82110000u) s_fake.fetch_add(1, std::memory_order_relaxed);
        else if (ms == 0) s_null.fetch_add(1, std::memory_order_relaxed);
        else s_real.fetch_add(1, std::memory_order_relaxed);
        const uint64_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 12 || (n % 20000) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[HEAP_MSTATE] #%llu mstate=0x%08X size=0x%X lr=0x%08X | "
                "fake(0x82110000)=%llu null=%llu real=%llu",
                (unsigned long long)n, ms, ctx.r4.u32, (uint32_t)ctx.lr,
                (unsigned long long)s_fake.load(std::memory_order_relaxed),
                (unsigned long long)s_null.load(std::memory_order_relaxed),
                (unsigned long long)s_real.load(std::memory_order_relaxed));
    }
    if (!guardEnabled()) {          // default exe: the guest's own malloc core, once
        __imp__sub_828CEBC0(ctx, base);
        return;
    }
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;
    const uint64_t r6 = ctx.r6.u64, r7 = ctx.r7.u64, lr = ctx.lr;
    for (unsigned tries = 0;; ++tries) {
        __imp__sub_828CEBC0(ctx, base);
        const uint32_t ret = ctx.r3.u32;
        if (ret == 0 || ret < kForbidLo || ret >= kForbidHi || !guardEnabled() ||
            tries >= 8) {
            if (tries > 0) {
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[HEAPCHUNK_GUARD] discarded %u poisoned chunk(s) in the "
                    "image/statics window — final alloc=%08X (size arg=%08X caller lr=%08X)",
                    tries, ret, (uint32_t)r4, (uint32_t)lr);
            }
            ctx.lr = lr;
            return;
        }
        static std::atomic<unsigned> s_logged{0};
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[HEAPCHUNK_GUARD] allocator returned chunk %08X INSIDE the "
                "Apt/AS statics window (size arg=%08X caller lr=%08X) — discarding, retrying",
                ret, (uint32_t)r4, (uint32_t)lr);
        }
        ctx.r3.u64 = r3;   // restore args for the retry
        ctx.r4.u64 = r4;
        ctx.r5.u64 = r5;
        ctx.r6.u64 = r6;
        ctx.r7.u64 = r7;
    }
}
