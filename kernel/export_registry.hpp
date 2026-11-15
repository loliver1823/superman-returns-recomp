#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sr {
namespace kernel {

// Xbox 360 PowerPC: first 6 args in r3-r8, return in r3 (32-bit).
// Stubs use this signature for logging; real impls may have specific types.
using ExportFn = uint32_t (*)(uint32_t r3, uint32_t r4, uint32_t r5,
                              uint32_t r6, uint32_t r7, uint32_t r8);

class ExportRegistry {
public:
  static ExportRegistry& instance();

  // Register export by name. Replaces existing.
  void registerExport(std::string_view name, ExportFn fn);

  // Lookup by name. Returns nullptr if not found.
  ExportFn getExport(std::string_view name) const;

  // Call export; if missing, log and return defaultVal. Never returns nullptr for fn.
  uint32_t callOrStub(std::string_view name, uint32_t r3, uint32_t r4,
                      uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8,
                      uint32_t defaultVal = 0) const;

  // Report all registered exports (for diagnostics)
  void listExports() const;

  // Instrumentation: set true before calling guest entry, false after. When true,
  // callOrStub logs every call with [GUEST] and a call index.
  static void setGuestCallContext(bool inGuest);

  // Guest idle heartbeat: last time (ms, monotonic) a kernel call was made from guest.
  static uint64_t getLastGuestCallTimeMs();
  static bool hasGuestCallInLastMs(uint64_t ms);
  static uint32_t getGuestCallCount();

  // Extra guest args not passed to ExportFn (r9/r10), captured per-thread by import glue.
  // Stubs that need >6 arguments (e.g. NtReadFile ByteOffset) can query these.
  static void setCurrentExtraArgs(uint32_t r9, uint32_t r10);
  static uint32_t getCurrentArgR9();
  static uint32_t getCurrentArgR10();
  // iter-463 (SR_KPCR): the calling guest context's r13 (its KPCR), published
  // per-thread by the import glue on every kernel call. Exports that need the
  // caller's KPCR (the Ke/Kf spinlocks: owner = r13) read it here.
  static void setCurrentR13(uint32_t r13);
  static uint32_t getCurrentR13();

private:
  ExportRegistry() = default;
  struct Impl;  // defined in .cpp
  static Impl& impl();
};

}  // namespace kernel
}  // namespace sr
