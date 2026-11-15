#pragma once
// step4/audit (docs/LOGIC_AUDIT.md 2, "which imports the routes call"): SR_AUDIT_IMPORTS, a COMPILE-TIME audit build
// (cmake -DSR_AUDIT_IMPORTS=ON in its own build tree; the release configuration refuses the option). Every kernel
// import call through the glue (guest/ppc_import_glue.cpp callKernel) is counted per export, in two halves:
//   ENTRY (before the export runs; after the quit gate's entry park): entries, the caller-lr histogram, the first
//          calls' arguments - so a call that never returns (ExTerminateThread, a wait that blocks to the end, a
//          faulting export, a thread parked at the quit's exit gate) is still counted;
//   RETURN (right after the export returns, before the quit gate's exit park): returns, calls with no registered
//          export, the result histogram, the first calls' results.
// entries - returns = calls in flight or never returned. A side thread writes the census as JSON every
// SR_AUDIT_IMPORTS_PERIOD seconds (default 10) to SR_AUDIT_IMPORTS_OUT (default %TEMP%\SupermanRecomp\
// import_audit_<pid>.json), and a final flush runs from BOTH std::atexit and std::at_quick_exit (the project's normal
// exits are quick_exit: app/main.cpp hostExitNow, kernel/quit.cpp), after stopping and joining the writer. The boot
// log names the file ([IMPORT_AUDIT], a player-log milestone tag). Data imports (the 10 variables) are not calls.
// PATHS (2026-10-20, logic audit 2.0g "the file opens"): every NtCreateFile / NtOpenFile (OBJECT_ATTRIBUTES r5) and
// NtQueryFullAttributesFile (r3) call also gets a record in call order - the export, lr, r4 (the desired access; for
// the attributes query the info pointer), the guest path as the title passed it (read at entry, before the export
// runs; first kPathLen-1 bytes) and the result - in the JSON's "paths" array (the first kPathRecs calls;
// "paths_dropped" counts the rest). The census's per-export records are unchanged.
// An audit build is never a proof, gate or release build: the census adds atomics to every import call.
// Without SR_AUDIT_IMPORTS this unit is not compiled and the glue has no hook.
#include <cstdint>

namespace sr {
namespace audit {

// What importEnter hands to importLeave (one import call on one thread).
struct ImportAuditToken {
  void* slot = nullptr;   // the export's census slot (null: the table was full)
  int first = -1;         // this call's index among the first calls kept, or -1
  int path = -1;          // this call's PATHS record, or -1 (not a path export, or the record table was full)
};

ImportAuditToken importEnter(const char* name, uint32_t lr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                             uint32_t r7, uint32_t r8);
// `resolved` = a registered export ran (false: the glue answered 0 without one).
void importLeave(const ImportAuditToken& token, uint32_t result, bool resolved);

// Test seams (tools/difftest/import_audit_test.cpp).
inline void importCall(const char* name, uint32_t lr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                       uint32_t r7, uint32_t r8, uint32_t result, bool resolved) {
  importLeave(importEnter(name, lr, r3, r4, r5, r6, r7, r8), result, resolved);
}
bool importAuditWriteJson(const char* path);   // the census now, as the side thread writes it
void importAuditResetForTest();                // forget every slot (single-threaded tests only)

}  // namespace audit
}  // namespace sr
