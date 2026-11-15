#include "native_hardware_memory.hpp"
#include "guest_protect.hpp"
#include <atomic>
#include <windows.h>

namespace sr::kernel {
namespace {
std::atomic<uintptr_t> g_hardwareStart{0};
bool protectHardwareReservation(uint8_t* hardware, uint32_t* error) noexcept {
  guest_protect::declare(hardware, kHardwareMemoryBytes, guest_protect::Scope::Range);
  DWORD previous = 0;
  if (!VirtualProtect(hardware, kHardwareMemoryBytes, PAGE_NOACCESS, &previous)) {
    if (error) *error = GetLastError();
    return false;
  }
  return true;
}
}
bool nativeHardwareMemoryIntersects(const void* address, size_t bytes) noexcept {
  const uintptr_t hardware = g_hardwareStart.load(std::memory_order_acquire);
  if (!hardware || !bytes) return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(address);
  if (bytes > UINTPTR_MAX - start) return true;
  return start < hardware + kHardwareMemoryBytes && start + bytes > hardware;
}
bool protectNativeHardwareMemory(uint8_t* base, size_t size, uint32_t* error) noexcept {
  if (error) *error = ERROR_SUCCESS;
  if (!base || size < size_t(kHardwareMemoryBase) + kHardwareMemoryBytes) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  if (!protectHardwareReservation(base + kHardwareMemoryBase, error)) return false;
  g_hardwareStart.store(reinterpret_cast<uintptr_t>(base + kHardwareMemoryBase), std::memory_order_release);
  return true;
}
}
