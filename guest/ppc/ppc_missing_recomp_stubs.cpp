// Hand-written strong definitions of guest functions the recompiler did not
// emit. Compiled as guest/ppc/ppc_missing_recomp_stubs.cpp (tools/regen/
// assemble.py copies this committed source there).
#include "ppc_config.h"
#include "ppc_context.h"
#include <atomic>
#include "../../app/log.hpp"
#include "../../kernel/guest_context.hpp"

// Stubs provide C++ linkage symbols to match PPC_EXTERN_FUNC declarations.
#define PPC_MISSING_STUB(name) PPC_FUNC(name) { (void)ctx; (void)base; }

// ========== iter-463 charter (burndown item 11): EVERY stub is DELETED ==========
// None of these addresses is a function start (IDA 9.1 over default.xex, and
// 0 references to any of them in all three golden dumps: archive/v1_burndown2/
// va_golden_refs.txt). They came from the heap-corruption era, when garbage
// function pointers (freed objects, byte-rotated vtables) landed on return
// addresses and mid-function code; each "unmapped stub call" got a stub that
// fabricated a plausible return. With the corruption fixed (60a90bf) a call to
// any of them is itself the bug, so it now reaches PPC_CALL_INDIRECT_FUNC's
// named log-and-fail ([ICALL_UNRESOLVED] / [ICALL_FAIL], kernel/indirect_call.cpp)
// instead of a fabricated result. Their dispatch entries and extern
// declarations are gone too (tools/regen/hand/extern_decls.txt).
//
// Link filler (no dispatch entry, no caller): sub_828D0EC8, sub_828D0F54,
//   sub_82765994.
// Return addresses / mid-function no-ops (reached, in the old logs, from the
//   luaD_throw panic call after a legacy longjmp RETURNED - "[LONGJMP] #" is in
//   0/150 run logs today): sub_827A177C (in sub_827A1690), sub_82762680 (in
//   sub_82762640), sub_82766C30 (in sub_82766BE0), sub_82766CA0 (in
//   sub_82766C78), sub_8276070C (in sub_827606E0), sub_827A60FC (in
//   MemAllocatorFreeBlock sub_827A60D0).
// Fake returns at mid-function VAs (the audit's FORCING list): sub_827C1D80
//   (bne; bl), sub_824B9560 (VMX code inside AudioScaleSamplesToNormalizedFloat),
//   sub_823A5198 (inside TexDitherPack2101010), sub_822F6F28 (skipped the
//   LZCOUNT check its own comment described), sub_8215BDF8 (a list insert),
//   sub_8215BE38 (lwz; mtctr; bctrl - not the claimed li r3,0; blr),
//   sub_8215BE40 (bctrl), sub_8215BEA8 (mr r3,r31; bl).
// The four guest/config/superman.toml "functions" entries (iter-27/28) went
//   with their stubs: sub_8246D5C8 (return address inside RtlFreeHeap),
//   sub_827AC440 (return address inside sub_827AC358), sub_8246D6C8 (branch
//   target inside sub_8246D650), sub_827A0000 (the CRT errno store inside
//   sub_8279FF48). The stubs dropped the generated bodies' `addi r1` + GPR
//   restore; the generated bodies themselves ended in "ERROR <tail>" returns
//   for 0x827A0078 / 0x8246DE64 - neither is a faithful entry, because none of
//   these VAs is an entry. A XenonRecomp run without the four entries changes
//   no other function (archive/v1_burndown2/regen/fndiff.txt: only those four
//   bodies, their mapping entries and their declarations disappear).
