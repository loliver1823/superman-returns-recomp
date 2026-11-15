// iter-463 PLATFORM FIX: Xenia-faithful MmGetPhysicalAddress. Contract: kernel/mm_phys.hpp.
// Ported semantics from Xenia Canary (BSD-3-Clause), canary_experimental@0bd090dbe,
// src/xenia/memory.cc and src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc.
#include "kernel/mm_phys.hpp"

#include "kernel/export_registry.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdlib>

namespace sr::kernel {

namespace {
std::atomic<int> g_testMode{-1};
std::atomic<uint32_t> g_logs{0};
}  // namespace

uint32_t xeGetPhysicalAddress(uint32_t address) {
  // memory.cc:441-462 LookupHeap. heap_base / physical-heap flag per memory.cc:211-232.
  uint32_t heap_base;
  bool physical_heap;
  if (address < 0x40000000u) {                 // v00000000 (kGuestVirtual)
    heap_base = 0x00000000u; physical_heap = false;
  } else if (address < 0x7F000000u) {          // v40000000 (kGuestVirtual)
    heap_base = 0x40000000u; physical_heap = false;
  } else if (address < 0x7FC80000u) {          // v7F000000 (kGuestPhysical)
    heap_base = 0x7F000000u; physical_heap = true;
  } else if (address < 0x80000000u) {          // no heap
    return UINT32_MAX;
  } else if (address < 0x90000000u) {          // v80000000 (kGuestXex)
    heap_base = 0x80000000u; physical_heap = false;
  } else if (address < 0xA0000000u) {          // v90000000 (kGuestXex)
    heap_base = 0x90000000u; physical_heap = false;
  } else if (address < 0xC0000000u) {          // vA0000000 (kGuestPhysical)
    heap_base = 0xA0000000u; physical_heap = true;
  } else if (address < 0xE0000000u) {          // vC0000000 (kGuestPhysical)
    heap_base = 0xC0000000u; physical_heap = true;
  } else if (address < 0xFFD00000u) {          // vE0000000 (kGuestPhysical)
    heap_base = 0xE0000000u; physical_heap = true;
  } else {
    return UINT32_MAX;
  }
  // memory.cc:532-535: "Assumption that we already received physical address, so just return it."
  if (!physical_heap && address < 0x1FFFFFFFu) {
    return address;
  }
  // memory.cc:537 + 2434-2442 (Xenia applies PhysicalHeap::GetPhysicalAddress to every heap
  // here; heap_base_ is a BaseHeap member, so the arithmetic below is exactly what runs).
  uint32_t pa = address - heap_base;
  if (heap_base >= 0xE0000000u) pa += 0x1000u;
  return pa;
}

uint32_t mmGetPhysicalAddressExport(uint32_t va) {
  uint32_t pa = xeGetPhysicalAddress(va);
  if (pa == UINT32_MAX) {
    const uint32_t n = g_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0)
      SR_LOG_KERNEL_IMPORT("[MM_PHYSADDR] #%u va=%08X has no heap -> 0 (Xenia assert path)", n, va);
    pa = 0;
  }
  return pa;
}

bool mmPhysAddrEnabled() {
  const int t = g_testMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_MM_PHYSADDR");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}

void mmPhysAddrSetEnabledForTest(int mode) { g_testMode.store(mode, std::memory_order_relaxed); }

void registerMmPhysExport(ExportRegistry& reg) {
  if (!mmPhysAddrEnabled()) return;
  reg.registerExport("MmGetPhysicalAddress",
                     +[](uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
                       return mmGetPhysicalAddressExport(r3);
                     });
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[MM_PHYSADDR] SR_MM_PHYSADDR=1: MmGetPhysicalAddress = Xenia GetPhysicalAddress");
}

}  // namespace sr::kernel
