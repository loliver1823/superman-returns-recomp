// Production SR_ORACLE=0 residency tracking. No packet decoder or guest runtime.
#include <windows.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include "kernel/native_const_dedup.hpp"
#include "app/log.hpp"

namespace {
uint8_t* commandBase = nullptr;
std::array<uint8_t, 13512> device{};
constexpr uint32_t deviceVA = 0x20000u;
void put32(size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) device[at + i] = uint8_t(value >> (24u - 8u * i));
}
int failures = 0;
void check(bool value, const char* name) {
  std::printf("[NATIVE_CONST_DEDUP_TEST] %s %s\n", value ? "PASS" : "FAIL", name);
  if (!value) ++failures;
}
}

// Only transport and logging are substituted; all residency decisions execute
// the production translation unit compiled with the shipping configuration.
namespace sr::kernel {
void getGlobalGuestMemory(uint8_t** base, size_t* bytes) { *base = commandBase; *bytes = 65536; }
uint8_t* guestVAToHost(uint32_t va) {
  return va >= deviceVA && va - deviceVA < device.size() ? device.data() + (va - deviceVA) : nullptr;
}
}
void sr::Log::write(LogCategory, LogSeverity, const char*, ...) {}

int main() {
  namespace cd = sr::native::cdedup;
  commandBase = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
  if (!commandBase) return 2;
  std::array<uint8_t, 64> values{};
  for (size_t i = 0; i < values.size(); ++i) values[i] = uint8_t(i * 7u);
  put32(13508, 0x1000u);
  put32(44, 0xF000u);
  check(cd::on() && !cd::checkOn(), "shipping mode");
  check(cd::syncStream(commandBase, deviceVA, device.data(), 0x1000u), "first producer");
  cd::store(0, values.data());
  cd::store(cd::kGroups - 1u, values.data());
  cd::noteEmitted(0x1100u);
  check(cd::syncStream(commandBase, deviceVA, device.data(), 0x1100u) &&
        cd::resident(0, values.data()) && cd::validGroups() == 2u, "same cursor retains groups");
  // The old adapter would dereference the PAGE_NOACCESS command memory here.
  check(cd::syncStream(commandBase, deviceVA, device.data(), 0x1104u) &&
        cd::validGroups() == 0u, "unknown advance invalidates without reading packets");
  cd::store(1, values.data());
  cd::noteEmitted(0x1200u);
  cd::advanceTo(deviceVA, 0x1204u);
  check(!cd::resident(1, values.data()), "external producer advance invalidates");
  cd::store(2, values.data());
  cd::noteEmitted(0x1300u);
  put32(13500, 1u);
  check(cd::syncStream(commandBase, deviceVA, device.data(), 0x1300u) &&
        !cd::resident(2, values.data()), "new epoch invalidates");
  cd::store(3, values.data());
  cd::noteEmitted(0x1400u);
  check(cd::syncStream(commandBase, deviceVA, device.data(), 0x1000u) &&
        !cd::resident(3, values.data()), "cursor rewind invalidates");
  VirtualFree(commandBase, 0, MEM_RELEASE);
  std::printf("[NATIVE_CONST_DEDUP_TEST] failures=%d\n", failures);
  return failures ? 1 : 0;
}
