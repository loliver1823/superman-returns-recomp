// D20 fixture: the generated indirect-call dispatcher hands unresolvable calls
// to a host helper with the guest context; that helper's register writes are
// guest results (kernel/helper.cpp).
#pragma once
#define PPC_CALL_INDIRECT_FUNC(x) do { \
    uint32_t _target = (x); \
    PPCFunc* _fn = lookup(_target); \
    if (_fn) { \
        _fn(ctx, base); \
    } else { \
        sr::kernel::fixtureUnresolved(ctx, _target); \
    } \
} while(0)
