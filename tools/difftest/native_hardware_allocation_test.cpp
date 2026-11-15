#include "kernel/native_hardware_memory.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/guest_protect.hpp"
#include "kernel/ntavm_xenia_heap.hpp"
#include <windows.h>
#include <cstdio>

namespace sr::kernel {
uint32_t stub_NtAllocateVirtualMemory(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
bool tryObserveAsfFree(uint32_t, uint32_t&, uint32_t&, uint32_t&);
}
namespace {
bool storeFaults(uint8_t* address) {
  __try { *static_cast<volatile uint8_t*>(address) = 0xA5; }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
  return false;
}
bool observeAsfFreeFaults(uint32_t a3, bool& observed, uint32_t& block, uint32_t& desc, uint32_t& dbase) {
  __try { observed = sr::kernel::tryObserveAsfFree(a3, block, desc, dbase); }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
  return false;
}
}

// The actual failed startup's reserve/commit pair through the production export,
// after applying release protection to difftest's ordinary Oracle arena.
int runNativeHardwareAllocationTest(bool oldRangeMutant) {
  namespace K = sr::kernel;
  unsigned failures = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
  };
  uint8_t* base = K::getGuestBase();
  uint32_t error = 0;
  if (!K::protectNativeHardwareMemory(base, 0x100000000ull, &error)) return 2;
  if (oldRangeMutant) {
    DWORD previous = 0;
    if (!VirtualProtect(base + 0x7E800000u, 0x01800000u, PAGE_NOACCESS, &previous)) return 2;
  }
  K::xheap::resetForTest();
  K::xheap::forceEnabledForTest(1);
  constexpr uint32_t addressCell = 0x10000u, sizeCell = 0x10004u;
  K::writeGuestBE32(addressCell, 0);
  K::writeGuestBE32(sizeCell, 0x11FFEEu);
  const uint32_t reserve = K::stub_NtAllocateVirtualMemory(addressCell, sizeCell, 0x20902000u, 4, 0, 0);
  const uint32_t granted = K::readGuestU32BE(addressCell);
  check(reserve == 0 && granted == 0x7EEE0000u && K::readGuestU32BE(sizeCell) == 0x120000u,
        "production top-down reserve retains the real 7EEE0000 grant");
  K::writeGuestBE32(sizeCell, 0x1FFEu);
  const uint32_t commit = K::stub_NtAllocateVirtualMemory(addressCell, sizeCell, 0x20901000u, 4, 0, 0);
  check(commit == 0 && K::readGuestU32BE(addressCell) == granted && K::readGuestU32BE(sizeCell) == 0x2000u,
        "production commit-in-place retains base and rounded size");
  check(!storeFaults(base + 0x7EEE1C70u), "actual boot heap write at 7EEE1C70 succeeds");
  check(!K::guest_protect::intersects(base + 0x7EEE1C70u, 1) &&
        !K::nativeHardwareMemoryIntersects(base + 0x7EEE1C70u, 1), "ordinary allocation is admitted");
  check(!storeFaults(base + 0x7F000000u), "GPU writeback RAM remains writable");
  check(!storeFaults(base + 0x7FC7FFFFu), "last writeback byte 7FC7FFFF remains writable");
  check(storeFaults(base + 0x7FC80000u), "first MMIO byte 7FC80000 rejects CPU writes");
  check(storeFaults(base + 0x7FC86530u), "scanout MMIO rejects CPU writes");
  // The actual FMV teardown failure was an observer's speculative descriptor
  // read. Exercise that production observer with valid data and inaccessible
  // links while hardware remains protected; the guest free body is untouched.
  constexpr uint32_t asfCell = 0x10010u, asfBlock = 0x92000000u, asfDesc = 0x10018u;
  uint32_t block = 0, desc = 0, dbase = 0;
  bool observed = false;
  K::writeGuestBE32(asfCell, asfBlock);
  K::writeGuestBE32(asfBlock, asfDesc);
  K::writeGuestBE32(asfDesc, 0x12345678u);
  check(!observeAsfFreeFaults(asfCell, observed, block, desc, dbase) && observed &&
        block == asfBlock && desc == asfDesc && dbase == 0x12345678u, "Oracle ASF observer preserves valid evidence");
  K::writeGuestBE32(asfBlock, 0x7FD0A999u);
  check(!observeAsfFreeFaults(asfCell, observed, block, desc, dbase) && !observed,
        "Oracle ASF observer rejects actual protected descriptor 7FD0A999 without a CPU fault");
  K::writeGuestBE32(asfCell, 0x7FD0A999u);
  check(!observeAsfFreeFaults(asfCell, observed, block, desc, dbase) && !observed,
        "Oracle ASF observer filters non-FMV block before chasing it");
  check(!observeAsfFreeFaults(0x7FC7FFFEu, observed, block, desc, dbase) && !observed,
        "Oracle ASF observer rejects a read crossing the MMIO boundary");
  check(storeFaults(base + 0x7FD0A999u), "ASF observation never reopens actual MMIO");
  check(K::xheap::release(granted) == 0x120000u, "production reservation releases exactly");
  std::printf("native hardware allocation: %u failures%s\n", failures, oldRangeMutant ? " (old-range mutant)" : "");
  return failures ? 1 : 0;
}
