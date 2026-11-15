// R1 (docs/SPLICE_CONVERSION.md): the hooks that replaced the platform, metric-observer and
// override-merged splices. The repository carries no recompiled game code; this is host code.
// Every port here keeps the behaviour of the old hand-edited body exactly (L6 golden-call
// differential against it); FORCING sites count their fires with R1_FIRE (kernel/r1_hooks.hpp).
// The other forcing ports live in kernel/r1_hooks_b1..b3.cpp; their toml entries are in
// tools/regen/superman_regen.toml.

#include "r1_hooks.hpp"
#include "guest_context.hpp"
#include <chrono>
#include <cstdlib>
#include <vector>

extern "C" void __imp__sub_82164818(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825FC108(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D1298(PPCContext& __restrict ctx, uint8_t* base);

// Crt_Longjmp (sub_827A4270): the hook that dropped `bne cr1,0x827a435c` at 0x827A429C (the RtlUnwind block
// 0x827A435C) is deleted (RELEASE_PLAN phase 4, docs/FORCING_RETIREMENT.md): the branch is taken only for a
// jmp_buf whose +0x138 unwind slot is non-zero, and Crt_Setjmp_sub_827A45E0 - the title's only setjmp - always
// writes 0 there (its alternative, a tail jump through the setjmp hook pointer dword_829B2FA8, has no writer in
// default.xex and reads 0 in the golden dumps). The generated body runs as on the console; it is reached only
// for a buffer no protected setjmp registered (kernel/r1_platform.cpp throws for every registered one).

// =====================================================================================
// Run-metric observers (tools/run_recomp_x.ps1 parses these log lines).
// =====================================================================================

// walksRet = the number of "POST_8271E928" markers: one per completed runner walk, logged by
// each of the four callers of SystemRunnerStart sub_8271E928 right after the call returns
// (after_instruction hooks on the four `bl 0x8271e928`).
void R1_sub_82177E60_82177EA8() { sr::kernel::logInit2Progress("sub_82177E60_POST_8271E928"); }
void R1_sub_82177F90_82178050() { sr::kernel::logInit2Progress("sub_82177F90_POST_8271E928"); }
void R1_sub_821780C0_82178178() { sr::kernel::logInit2Progress("sub_821780C0_POST_8271E928"); }
void R1_sub_821781F8_821782B8() { sr::kernel::logInit2Progress("sub_821781F8_POST_8271E928"); }

// walkDepth = the largest call number in "E928_m_E630_enter: val1=<call#>": sub_8271E928
// numbers its calls (kernel/rtam_gameconfig_watch.cpp brackets the body with r1E928Enter /
// r1E928Exit) and logs before its call into sub_8271E630 (hook on `bl 0x8271e630`, 0x8271E9D4).
namespace {
thread_local std::vector<unsigned> t_e928Calls;
unsigned s_e928CallCount = 0;
}  // namespace
namespace sr::r1 {
void e928Enter() { t_e928Calls.push_back(++s_e928CallCount); }
void e928Exit() {
    if (!t_e928Calls.empty())
        t_e928Calls.pop_back();
}
}  // namespace sr::r1
void R1_sub_8271E928_8271E9D4(PPCRegister& r30) {
    const unsigned cc = t_e928Calls.empty() ? 0u : t_e928Calls.back();
    sr::kernel::logExThreadPathBranch("E928_m_E630_enter", cc, r30.u32);
}

// lastInit = the module of the last "F2C8_vt7_pre" line: sub_8271F2C8 logs the module (r31)
// and its vtable[7] target (ctr) right before that bctrl (0x8271F32C).
void R1_sub_8271F2C8_8271F32C(PPCRegister& r31, PPCRegister& ctr) {
    sr::kernel::logExThreadPathBranch("F2C8_vt7_pre", r31.u32, ctr.u32);
}

// sub_826D1298 stores the Apt environment pointer (r3) to its global; the old splice also
// published it to sr::kernel::g_aptEnvVA, which only the SR_STORE_WATCHPOINTS=1 Apt-env
// store watch reads. Observe-only.
PPC_FUNC(sub_826D1298) {
    const uint32_t env = ctx.r3.u32;
    __imp__sub_826D1298(ctx, base);
    sr::kernel::g_aptEnvVA.store(env, std::memory_order_relaxed);
}

// =====================================================================================
// FORCING ports merged into functions that already had a kernel override (the override
// calls the helper in place of / around its __imp__ call). Behaviour unchanged.
// =====================================================================================
namespace sr::r1 {

// (The l_realloc SR_REALLOC_GUARD entry refusal and the luaD_call degenerate-lua_State entry guard
// that lived here were retired in RELEASE_PLAN phase 4.3, batch Lua, docs/FORCING_RETIREMENT.md;
// so were the l_realloc null-pool and luaD_call outside-code hooks.)

// (The ResTaskProcessOnLoad null-this entry guard that lived here was retired in RELEASE_PLAN phase 4.3,
// batch 3, docs/FORCING_RETIREMENT.md.)


// LuaFreeProtoTreeRecursive (sub_82164B60): the R1 work-list rewrite (protoStripIterative) was
// retired in phase 4 row 9 (docs/FORCING_RETIREMENT.md); the generated recursion runs under the
// [PROTO_STRIP] observe override in kernel/cs_timeout_underflow_fix.cpp.

}  // namespace sr::r1

// CsEnterWithDeadline (sub_825FC108): the 30 s wall-clock TryEnter escape was retired in RELEASE_PLAN phase 4.3,
// batch 4 (docs/FORCING_RETIREMENT.md): the guest's own deadline runs on the 50 MHz timebase.
