// D16 fixture: the generated-code indirect-call dispatcher. Every generated
// `bctrl` expands this macro, so a branch that does NOT dispatch the guest's
// target (a fabricated r3, a host stub standing in for guest code) runs from
// guest code in the default exe.
#pragma once
#include <cstdint>
#define PPC_FAKE_HASH_ENTRY_VA 0x822979C4u
#ifndef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) do { \
    uint32_t _target = (x); \
    if (_target == 0) { \
        ctx.r3.u32 = 0; /* EXPECT: UNGATED */ \
    } else if (_target == 0x82100400u) { \
        uint8_t* _base = PPC_GUEST_BASE(); \
        PPC_allocator_slot0_stub(ctx, _base); /* EXPECT: UNGATED */ \
    } else if (_target == PPC_FAKE_HASH_ENTRY_VA && sr::kernel::forcingAllowed()) { \
        ctx.r3.u32 = 0; /* EXPECT: MASTER */ \
    } else if (_target >= PPC_CODE_BASE && _target < (PPC_CODE_BASE + PPC_CODE_SIZE)) { \
        PPCFunc* _fn = PPC_LOOKUP_FUNC(base, _target); \
        uint8_t* _base = PPC_GUEST_BASE(); \
        if (_fn) { _fn(ctx, _base); } \
        else { ctx.r3.u32 = 0; } /* EXPECT: UNGATED */ \
    } else { \
        ctx.r3.u32 = 0; /* EXPECT: UNGATED */ \
    } \
} while(0)
#endif

// Control: a register write OUTSIDE the macro (an inline helper) is not a
// dispatcher branch.
inline void resetR3(PPCContext& ctx) { ctx.r3.u32 = 0; }
