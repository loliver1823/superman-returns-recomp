// R1 (docs/SPLICE_CONVERSION.md): shared conventions for the host-side hooks that replaced the
// hand-edited ("spliced") recompiled bodies. The repository carries no recompiled game code;
// every behaviour the splices added now lives in a kernel strong-def override or a
// tools/regen/superman_regen.toml [[midasm_hook]] whose C++ side lives in kernel/r1_hooks_*.cpp.
//
// R1 is mechanical: each hook reproduces its splice's behaviour exactly (proved by the L6
// golden-call differential against the old splice body). Every FORCING hook - a guard, cap,
// wall-clock exit or fake result the console never had - counts its fires with R1_FIRE so the
// band-aid burndown that follows R1 can retire each one on evidence (0 fires across N whole-run
// rolls), not argument. Each is registered as a `band-aid` in docs/OVERRIDES.json with its
// retirement condition.
//
// Guest memory: use the generated code's own PPC_LOAD_* / PPC_STORE_* macros (outside a
// generated function they read the thread's guest base through TLS), so a hook touches guest
// memory exactly the way the splice body did.
#pragma once

#include "guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"
#include <atomic>
#include <cstdint>

namespace sr::r1 {

// One per hook site. The first fire logs one line; after that a line at every power of two
// (2, 4, 8, ...), so any run log gives the fire count to within a factor of two at no cost
// on the hot path.
struct FireSite {
    const char* name;
    std::atomic<uint64_t> n{0};
};

inline void fired(FireSite& s, uint32_t a, uint32_t b) {
    const uint64_t n = s.n.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[R1_FIRE] %s #%llu a=%08X b=%08X",
                    s.name, (unsigned long long)n, a, b);
}

// The .text range the "indirect target outside the code" guards test (the splices used
// PPC_CODE_BASE / PPC_CODE_SIZE from ppc_config.h).
inline bool inCode(uint32_t va) {
    return va >= (uint32_t)PPC_CODE_BASE && va < (uint32_t)(PPC_CODE_BASE + PPC_CODE_SIZE);
}

// Ports merged into existing kernel overrides (definitions: kernel/r1_hooks_core.cpp).
void e928Enter();                            // sub_8271E928 call numbering (walkDepth)
void e928Exit();

// Runs the generated body of a function that calls Crt_Setjmp so that a guest longjmp to the
// buffer it armed resumes after its setjmp call (kernel/r1_platform.cpp; one of the seven
// setjmp users there, or the body just runs).
void setjmpProtected(PPCContext& ctx, uint8_t* base, uint32_t fn, void (*body)(PPCContext&, uint8_t*));

// Crt_Longjmp's host half (kernel/r1_platform.cpp): for a registered buffer, the console's
// _NLG_Notify call, then the SrGuestLongjmp throw; false (and nothing done) for any other buffer.
bool crtLongjmpThrow(PPCContext& ctx, uint8_t* base);

}  // namespace sr::r1

// Crt_Longjmp throws this for a jump buffer registered by luaD_rawrunprotected.
extern "C" bool srGuestLongjmpTryThrow(uint32_t bufVA, uint32_t code);

// R1_FIRE("sub_XXXXXXXX@0xADDR what", a, b): count one fire of this hook site.
#define R1_FIRE(name, a, b)                                   \
    do {                                                      \
        static ::sr::r1::FireSite r1_site_{name};             \
        ::sr::r1::fired(r1_site_, (uint32_t)(a), (uint32_t)(b)); \
    } while (0)
