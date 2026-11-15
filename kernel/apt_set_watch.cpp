// iter-297e: MovieClip host-setter watch — WHY does "onEnterFrame" never wire?
//
// sub_826BBDA0 = the MovieClip property setter (interpreter SetMember/
// SetVariable on clip-bound objects routes here). Its event-name cases
// (property-descriptor IDs 0xC8-0xD6 via Function_8267F550) OR the handler
// mask into *(wrapper+0xc)+0x10 and add the clip to the exec-ctx enter-frame
// list. ICONMENU's master script assigns `onEnterFrame = <anon fn>` yet the
// mask stays 0 (iter-297d, 5 attempts) — either this setter is never entered
// for that assignment (SetMember not routing to the host object), or the
// descriptor lookup misses. This override logs event-family names ("on*")
// plus the mask before/after, so one drain run names the exact miss.
// SR_SET_WATCH=0 disables logging.
//
// Contract (Ghidra): r3=vm/env?, r4=clip OBJECT (obj[8]=wrapper ptr),
// r5=&nameObj (*r5 -> {+2 u16 len, +8 chars}), r6=value obj.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>

#include "sr_oracle.hpp"

#if SR_ORACLE
extern "C" void __imp__sub_826BBDA0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::atomic<uint32_t> g_setCalls{0};
bool watchEnabled()
{
    static const bool v = [] {
        const char* e = std::getenv("SR_SET_WATCH");
        return !e || e[0] != '0';   // default ON; =0 keeps the A/B
    }();
    return v;
}
uint32_t rd(uint8_t* base, uint32_t va)
{
    if (va < 0x10000u || va >= 0xC0000000u) return 0;
    return sr::kernel::readGuestU32BE(va);
}
} // namespace

PPC_FUNC(sub_826BBDA0)
{
    const uint32_t n = g_setCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t objVA  = ctx.r4.u32;
    const uint32_t nameP  = ctx.r5.u32;
    const uint32_t valVA  = ctx.r6.u32;
    char name[28] = {};
    bool evName = false;
    uint32_t nameObj = rd(base, nameP);
    if (nameObj) {
        uint16_t len = 0;
        if (nameObj + 4 < 0xC0000000u) {
            len = static_cast<uint16_t>(
                (base[nameObj + 2] << 8) | base[nameObj + 3]);
        }
        uint32_t cap = len < 27 ? len : 27;
        for (uint32_t i = 0; i < cap; ++i) name[i] = static_cast<char>(base[nameObj + 8 + i]);
        evName = name[0] == 'o' && name[1] == 'n';
    }
    // wrapper event mask: *(*(obj+0x20)+0xc)+0x10 (the setter's own target)
    uint32_t wrapper = rd(base, objVA + 0x20u);
    uint32_t fs      = wrapper ? rd(base, wrapper + 0xcu) : 0;
    uint32_t maskB   = fs ? rd(base, fs + 0x10u) : 0;
    __imp__sub_826BBDA0(ctx, base);
    if (watchEnabled() && (evName || n <= 12)) {
        uint32_t maskA = fs ? rd(base, fs + 0x10u) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[SET_WATCH] #%u obj=%08X wrapper=%08X name='%s' val=%08X "
            "mask %08X -> %08X%s",
            n, objVA, wrapper, name, valVA, maskB, maskA,
            (evName && maskA == maskB) ? "  (EVENT NAME, MASK UNCHANGED)" : "");
    }
}

#endif  // Oracle-only property diagnostics
