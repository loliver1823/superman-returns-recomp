// D9 fixture: "dead code: no reference" is a fallback that hides live code.
// A function referenced only from a file-scope table (verifier probes3 fnB) or
// a static initializer, and an override of a guest function (called from
// GENERATED code the reference index does not read), both run with no gate.
#include <cstdint>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };

// UNSAFE-direction case: reached through a callback table.
void fabTable() { sr::kernel::writeGuestBE32(0x10u, 1u); }  // EXPECT: UNGATED
static void (*s_table[])() = { fabTable };
void runTable() { for (auto f : s_table) f(); }

// UNSAFE-direction case: reached through a file-scope pointer initializer (the
// reference sits outside every function, which read as "no reference").
void fabHook() { sr::kernel::writeGuestBE32(0x18u, 4u); }  // EXPECT: UNGATED
static void (*const kHook)() = &fabHook;
void runHook() { kHook(); }

// UNSAFE-direction case: a guest-called override.
void sub_82000090(PPCContext& ctx, uint8_t* base) {
  sr::kernel::writeGuestBE32(ctx.r3.u32, 2u);  // EXPECT: UNGATED
}

// A PPC_FUNC-defined override is sub_X (not a function named "PPC_FUNC").
#define PPC_FUNC(x) void x(PPCContext& ctx, uint8_t* base)
PPC_FUNC(sub_820000A0) {
  sr::kernel::writeGuestBE32(ctx.r3.u32, 5u);  // EXPECT: UNGATED fn=sub_820000A0
}

// A hand-written guest-callable function (PPCContext& signature) reached through
// a dispatch table / fake VA (ppc_import_glue.cpp PPC_allocator_slot0_stub).
void PPC_fabStub(PPCContext& ctx, uint8_t*) {
  sr::kernel::writeGuestBE32(ctx.r3.u32, 6u);  // EXPECT: UNGATED
}

// Control: a helper nothing references stays dead code.
static void neverCalled() { sr::kernel::writeGuestBE32(0x14u, 3u); }  // EXPECT: OWNGATE
