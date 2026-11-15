#include "harness.hpp"
#include "export_registry.hpp"
#include "stubs.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

namespace sr {
namespace kernel {

bool runHarness() {
  SR_LOG_BOOT(sr::LogSeverity::Info, "Kernel harness: registering stubs");
  registerStubs();

#if SR_ORACLE
  // The registry self-test below (the export listing, four registered stubs and one missing export called with dummy
  // arguments) is a development check. SR_ORACLE=0 (kernel/sr_oracle.hpp, the release configuration) keeps only the
  // registration: the game's imports resolve through registerStubs(). The dummy calls leave nothing the game reads -
  // guest memory is not mapped yet (runGuestHarness maps it later), so they write no guest memory; the one lasting
  // effect is KeInitializeEvent's host event for VA 0x2000, a VA no guest object uses.
  auto& reg = ExportRegistry::instance();
  reg.listExports();

  SR_LOG_BOOT(sr::LogSeverity::Info, "Kernel harness: calling exports");

  // Call registered stubs with dummy args
  uint32_t r1 = reg.callOrStub("NtAllocateVirtualMemory", 0, 0, 0, 0, 0, 0);
  uint32_t r2 = reg.callOrStub("KeQueryPerformanceCounter", 0x1000, 0, 0, 0, 0, 0);
  uint32_t r3 = reg.callOrStub("KeInitializeEvent", 0x2000, 1, 0, 0, 0, 0);
  uint32_t r4 = reg.callOrStub("RtlAllocateHeap", 0, 0x100, 0, 0, 0, 0);

  SR_LOG_KERNEL_IMPORT("Harness results: NtAlloc=%08X KeQuery=%08X KeInit=%08X RtlAlloc=%08X",
                       r1, r2, r3, r4);

  // Call missing export - must report cleanly, not crash
  SR_LOG_BOOT(sr::LogSeverity::Info, "Kernel harness: calling missing export (should log MISSING)");
  uint32_t missingRet = reg.callOrStub("NonexistentExport", 0xDEAD, 0xBEEF, 0, 0, 0, 0, 0xFFFFFFFF);
  SR_LOG_KERNEL_IMPORT("Missing export returned: %08X (expected stub default)", missingRet);

  SR_LOG_BOOT(sr::LogSeverity::Info, "Kernel harness: done");
#endif
  return true;
}

}  // namespace kernel
}  // namespace sr
