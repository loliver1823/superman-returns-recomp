// iter-296d: native side of the TOML [[midasm_hook]] entries in
// tools/regen/superman_regen.toml â€” the first hooks through the regeneration
// pipeline.
//
// SweepGuard (sub_82763628, the allocator deferred-free sweep):
// The FE_TICK thread was sampled for minutes with lr frozen at 0x82763630 â€”
// that lr is only live inside the sweep's list-walk loop, whose skip path
// (node flags fail) contains no calls. A corrupted/cyclic list therefore
// spins the thread forever. The iteration hook runs Brent's cycle detection
// per thread; on a PROVEN cycle (or a 4M-iteration blowout) it logs loudly
// and â€” unless SR_SWEEP_GUARD=0 â€” returns true, which the recompiler turns
// into a jump to the natural loop-exit block at 0x827636C0 (runs the real
// epilogue, re-links the list tail, frees what was already accumulated).
// The guard bounds each sweep; the cycle's CREATOR is the follow-up hunt.

#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"
#include "guest_context.hpp"
#include <cstdlib>
#include <cstdint>

namespace {

struct SweepState {
    uint32_t iters = 0;
    uint32_t tortoise = 0;   // Brent's algorithm
    uint32_t power = 1;
    uint32_t lam = 0;
    bool reported = false;
};
thread_local SweepState t_sweep;

// iter-463 charter (burndown item 4): SR_SWEEP_GUARD / SR_STATICFILL_GUARD /
// SR_PICKITER_GUARD shipped DEFAULT-ON. All three change guest control flow
// (bail out of a guest loop, cut a guest list link) - band-aids for the
// heap-corruption family 60a90bf fixed - and none fired in a recorded run:
// "[SWEEP] sub_82763628" 0/150, "[SWEEP2]" 0/150, "[STATICFILL]" 0/150,
// "[PICKITER]" 0/150 run logs (81 before 60a90bf, 69 after). They now act
// only under SR_ALLOW_FORCING; the default exe still LOGS the pathology.
bool sweepGuardEnabled()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_SWEEP_GUARD");
        return !e || e[0] != '0';   // =0 keeps the A/B (under SR_ALLOW_FORCING)
    }();
    return v && sr::kernel::forcingAllowed();
}

constexpr uint32_t kBlowoutIters = 4u << 20;   // no sane sweep walks 4M nodes

} // namespace

// StaticFillGuard (sub_826F5970, vector assign/fill): on bad DOGMA rolls a
// worker-dispatched rehash runs the fill with a buffer overlapping the
// Apt/AS static globals â€” caught zero-filling the importCb slot 0x829A1AC0
// (watchpoint, run1 2026-07-02 20:10), which scribbles the AS-intern
// buckets and breaks every subsequent register attempt. r11 = destination
// buffer (just loaded from vec+0), r31 = word count, r29 = &fill value.
// On overlap with the static-globals window, bail to the natural loop exit
// 0x826F59B8 (vector ends up empty but consistent). SR_STATICFILL_GUARD=0
// keeps the A/B.
bool StaticFillGuardMidAsmHook826F5970(PPCRegister& r11, PPCRegister& r31, PPCRegister& r29)
{
    const uint32_t buf = r11.u32;
    const uint32_t count = r31.u32;
    if (count == 0)
        return false;
    const uint64_t end = (uint64_t)buf + 4ull * count;
    constexpr uint32_t kStaticLo = 0x82920000u;   // start of data/BSS globals
    constexpr uint32_t kStaticHi = 0x829C0000u;   // end of the window we protect
    if (buf >= kStaticHi || end <= kStaticLo)
        return false;
    static const bool guard = [] {
        const char* e = std::getenv("SR_STATICFILL_GUARD");
        return (!e || e[0] != '0') && sr::kernel::forcingAllowed();
    }();
    static thread_local uint32_t s_lastBuf = 0;
    if (buf != s_lastBuf) {
        s_lastBuf = buf;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[STATICFILL] sub_826F5970 fill INTO STATIC WINDOW: "
                "buf=%08X count=%u (end=%08llX) valPtr=%08X - %s",
                buf, count, (unsigned long long)end, r29.u32,
                guard ? "BLOCKED (bailing to loop exit)" : "allowed (guard off: SR_ALLOW_FORCING / SR_STATICFILL_GUARD)");
    }
    return guard;
}

// PickIterGuard (sub_8219F7A8, screen-object pick iterator, 9 callers):
// r29 = container+8 ({begin,end} of 8-byte (id,obj) entries), r27 = entry
// count computed at entry. The loop re-reads the live end pointer and takes
// a call-free leaf path for null entries â€” a corrupt begin/end spins the
// FE_TICK thread forever (sampled lr=0x8219F7B0). Log suspicious counts;
// bail (jump to the clean exit at 0x8219F968) on absurd ones unless
// SR_PICKITER_GUARD=0.
bool PickIterGuardMidAsmHook8219F7A8(PPCRegister& r29, PPCRegister& r27)
{
    const uint32_t count = r27.u32;
    if (count <= 10000)
        return false;
    static thread_local uint32_t s_lastLogged = 0;
    if (count != s_lastLogged) {
        s_lastLogged = count;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PICKITER] sub_8219F7A8 container=%08X count=%u%s",
                r29.u32 - 8, count,
                count > (1u << 20) ? " ABSURD (bails to the exit block only under SR_ALLOW_FORCING)" : " (suspicious, allowed)");
    }
    if (count > (1u << 20)) {
        static const bool guard = [] {
            const char* e = std::getenv("SR_PICKITER_GUARD");
            return (!e || e[0] != '0') && sr::kernel::forcingAllowed();   // =0 keeps the A/B
        }();
        return guard;
    }
    return false;
}

void SweepEnterMidAsmHook82763628(PPCRegister& r26)
{
    (void)r26;
    t_sweep = SweepState{};
}

bool SweepIterMidAsmHook82763628(PPCRegister& r26, PPCRegister& r31)
{
    SweepState& s = t_sweep;
    ++s.iters;

    const uint32_t node = r31.u32;
    bool cycle = false;
    if (node == s.tortoise && s.iters > 1)
        cycle = true;
    if (s.lam == s.power) {
        s.tortoise = node;
        s.power <<= 1;
        s.lam = 0;
    }
    ++s.lam;

    if (cycle || s.iters >= kBlowoutIters) {
        if (!s.reported) {
            s.reported = true;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[SWEEP] sub_82763628 PATHOLOGICAL: %s heap=%08X node=%08X "
                    "iters=%u lam=%u (guard %s)",
                    cycle ? "LIST CYCLE (Brent-proven)" : "4M-iteration blowout",
                    r26.u32, node, s.iters, s.lam,
                    sweepGuardEnabled() ? "BAILING to natural exit" : "OFF - will spin");
        }
        if (sweepGuardEnabled()) {
            t_sweep = SweepState{};   // fresh state for the next sweep call
            return true;              // jump_address_on_true -> 0x827636C0
        }
    }
    return false;
}

// iter-365l: SIBLING sweep sub_827635D8 — same deferred-free family as the
// guarded sub_82763628 above, but a separate loop the midasm guard does not
// cover: walks list head *(*(this+16)+20), clears bit0 of node+5, next=*node
// (in-loop lr=0x82763608 — the exact stall site once the conveyor-pace flip
// clears the effect-preload wall). A cyclic list spins it forever. This one is
// a whole function, so guard it kernel-side: Floyd-detect the cycle over the
// next pointers, CUT the closing link (write 0), then run the original walk
// (now finite). Same SR_SWEEP_GUARD gate; the cycle CREATOR remains the
// follow-up hunt (same corruption family as iter-296d).
extern "C" void __imp__sub_827635D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827635D8(PPCContext& __restrict ctx, uint8_t* base)
{
    if (sweepGuardEnabled()) {
        const uint32_t self = ctx.r3.u32;
        const uint32_t p16 = (self >= 0x10000u) ? sr::kernel::readGuestU32BE(self + 16) : 0;
        const uint32_t head = (p16 >= 0x10000u) ? sr::kernel::readGuestU32BE(p16 + 20) : 0;
        auto nxt = [](uint32_t n) -> uint32_t {
            return (n >= 0x10000u && n < 0xC0000000u) ? sr::kernel::readGuestU32BE(n) : 0;
        };
        if (head >= 0x10000u) {
            uint32_t slow = head, fast = head, iters = 0;
            bool cycle = false;
            for (;;) {
                slow = nxt(slow);
                fast = nxt(nxt(fast));
                if (!slow || !fast) break;                    // clean termination
                if (slow == fast) { cycle = true; break; }
                if (++iters >= kBlowoutIters) break;          // give up detecting
            }
            if (cycle) {
                // find cycle start, then the node whose next closes the loop
                uint32_t a = head;
                while (a != slow) { a = nxt(a); slow = nxt(slow); }
                uint32_t last = a;
                while (nxt(last) != a) last = nxt(last);
                if (uint8_t* h = sr::kernel::guestVAToHost(last)) {
                    h[0] = h[1] = h[2] = h[3] = 0;            // next = NULL (BE zero)
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[SWEEP2] sub_827635D8 LIST CYCLE (Floyd-proven): head=%08X "
                        "cycleStart=%08X cutNode=%08X — link CUT, walk now finite",
                        head, a, last);
                }
            }
        }
    }
    __imp__sub_827635D8(ctx, base);
}
