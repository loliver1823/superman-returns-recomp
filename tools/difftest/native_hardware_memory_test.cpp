#include "kernel/native_hardware_memory.hpp"
#include "kernel/guest_protect.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
unsigned failures = 0;
void check(bool condition, const char* message) {
  if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
bool readable(const uint8_t* address) {
  uint8_t byte = 0;
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), address, &byte, 1, &got) && got == 1;
}
bool writeFaults(uint8_t* address) {
  __try { *static_cast<volatile uint8_t*>(address) = 0xA5; }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
  return false;
}
bool readFaults(const uint8_t* address) {
  __try { volatile uint8_t value = *static_cast<const volatile uint8_t*>(address); (void)value; }
  __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
  return false;
}
}

int main(int argc, char** argv) {
  using namespace sr::kernel;
  const bool omitProtection = argc == 2 && std::strcmp(argv[1], "--negative-no-protection") == 0;
  constexpr size_t size = 0x100000000ull;
  auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
  if (!base) return 2;
  uint8_t* hardware = base + kHardwareMemoryBase;
  if (!VirtualAlloc(hardware - 4096, kHardwareMemoryBytes + 8192, MEM_COMMIT, PAGE_READWRITE)) {
    VirtualFree(base, 0, MEM_RELEASE);
    return 2;
  }
  uint32_t error = 123;
  check(!protectNativeHardwareMemory(nullptr, size, &error) && error == ERROR_INVALID_PARAMETER, "null arena refused");
  check(!protectNativeHardwareMemory(base, kHardwareMemoryBase, &error) && error == ERROR_INVALID_PARAMETER,
        "truncated arena refused");
  check(readable(hardware) && readable(base + 0x7FC86530u), "negative control starts readable");
  if (!omitProtection) check(protectNativeHardwareMemory(base, size, &error) && error == ERROR_SUCCESS, "protect succeeds");
  for (uint32_t va : {kHardwareMemoryBase, 0x7FC80714u, 0x7FC86530u, 0x7FC86544u, 0x7FC86584u, 0x7FFFFFFFu}) {
    uint8_t* p = base + va;
    MEMORY_BASIC_INFORMATION mbi{};
    check(VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_NOACCESS,
          "hardware address is committed no-access");
    check(guest_protect::intersects(p, 1), "arithmetic admission excludes protected address");
    check(nativeHardwareMemoryIntersects(p, 1), "diagnostic reprotection rejects hardware address");
    check(!readable(p), "OS hardware read rejected");
    check(readFaults(p), "CPU hardware read raises access violation");
    check(writeFaults(p), "CPU hardware write raises access violation");
  }
  check(readable(hardware - 1) && readable(hardware + kHardwareMemoryBytes), "adjacent RAM remains readable");
  check(!guest_protect::intersects(hardware - 1, 1) && !guest_protect::intersects(hardware + kHardwareMemoryBytes, 1),
        "adjacent RAM remains admitted");
  check(!writeFaults(hardware - 1) && !writeFaults(hardware + kHardwareMemoryBytes), "adjacent RAM remains writable");
  check(guest_protect::intersects(hardware - 1, 2), "crossing range is excluded");
  check(nativeHardwareMemoryIntersects(hardware - 1, 2), "diagnostic reprotection rejects crossing range");
  check(!nativeHardwareMemoryIntersects(hardware - 1, 1) &&
        !nativeHardwareMemoryIntersects(hardware + kHardwareMemoryBytes, 1),
        "diagnostic reprotection admits adjacent RAM");
  check(nativeHardwareMemoryIntersects(reinterpret_cast<void*>(UINTPTR_MAX - 1), 4),
        "diagnostic reprotection refuses wrapping range");
  VirtualFree(base, 0, MEM_RELEASE);
  std::printf("native hardware memory: %u failures%s\n", failures, omitProtection ? " (negative control)" : "");
  return failures ? 1 : 0;
}
