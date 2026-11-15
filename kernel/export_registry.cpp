#include "export_registry.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <mutex>

namespace sr {
namespace kernel {

namespace {
  std::atomic<bool> g_inGuestContext{false};
  std::atomic<uint32_t> g_guestCallCount{0};
  std::atomic<uint64_t> g_lastGuestCallTimeMs{0};
  thread_local uint32_t g_guestArgR9 = 0;
  thread_local uint32_t g_guestArgR10 = 0;
  thread_local uint32_t g_guestR13 = 0;

  uint64_t nowMs() {
    return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
}

struct ExportRegistry::Impl {
  std::unordered_map<std::string, ExportFn> exports;
  mutable std::mutex mutex;
};

void ExportRegistry::setGuestCallContext(bool inGuest) {
  g_inGuestContext.store(inGuest);
  if (!inGuest) g_guestCallCount.store(0);
}

ExportRegistry::Impl& ExportRegistry::impl() {
  static Impl s_impl;
  return s_impl;
}

ExportRegistry& ExportRegistry::instance() {
  static ExportRegistry s_registry;
  return s_registry;
}

void ExportRegistry::registerExport(std::string_view name, ExportFn fn) {
  std::lock_guard<std::mutex> lock(impl().mutex);
  impl().exports[std::string(name)] = fn;
}

ExportFn ExportRegistry::getExport(std::string_view name) const {
  std::lock_guard<std::mutex> lock(impl().mutex);
  auto it = impl().exports.find(std::string(name));
  if (it != impl().exports.end()) {
    return it->second;
  }
  return nullptr;
}

uint32_t ExportRegistry::callOrStub(std::string_view name, uint32_t r3,
                                    uint32_t r4, uint32_t r5, uint32_t r6,
                                    uint32_t r7, uint32_t r8,
                                    uint32_t defaultVal) const {
  const bool fromGuest = g_inGuestContext.load(std::memory_order_relaxed);
  const uint32_t callNo = fromGuest ? g_guestCallCount.fetch_add(1, std::memory_order_relaxed) + 1u : 0u;
  if (fromGuest)
    g_lastGuestCallTimeMs.store(nowMs(), std::memory_order_relaxed);

  ExportFn fn = getExport(name);
  uint32_t result;
  if (fn) {
    result = fn(r3, r4, r5, r6, r7, r8);
    if (fromGuest) {
      // Filter out very frequent calls to reduce log noise
      bool shouldLog = true;
      if (name == "RtlEnterCriticalSection" || name == "RtlLeaveCriticalSection" ||
          name == "KeTlsGetValue" || name == "KeTlsSetValue" || name == "RtlInitializeCriticalSection" ||
          name == "MmAllocatePhysicalMemoryEx" || name == "KeBugCheck" || name == "KeBugCheckEx" ||
          name == "KeGetCurrentProcessType" || name == "NtYieldExecution" ||
          name == "KeDelayExecutionThread" ||
          // Render-loop calls (~60/sec each) – VdSwap/VdRetrainEDRAM log milestones in their stubs
          name == "VdSwap" || name == "VdGetSystemCommandBuffer" || name == "VdRetrainEDRAM" ||
          name == "KeWaitForSingleObject" || name == "KeEnterCriticalRegion" ||
          name == "KeLeaveCriticalRegion" || name == "MmFreePhysicalMemory" ||
          name == "KeResetEvent" || name == "KeSetEvent" || name == "KeQueryPerformanceCounter" ||
          name == "NtWaitForSingleObjectEx" || name == "NtReleaseSemaphore" ||
          name == "NtCreateSemaphore" || name == "NtDuplicateObject" ||
          name == "RtlTryEnterCriticalSection" || name == "RtlEnterCriticalSection" ||
          name == "RtlLeaveCriticalSection" || name == "NtYieldExecution" ||
          name == "KeTlsGetValue" || name == "KeTlsSetValue" ||
          name == "ObReferenceObjectByHandle" || name == "ObDereferenceObject" ||
          name == "NtQueueApcThread" || name == "NtResumeThread") {
        shouldLog = false;  // High-frequency calls; stubs log milestones internally
      }
      if (shouldLog) {
        SR_LOG_KERNEL_IMPORT("[GUEST #%u] %.*s (r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) -> %08X",
                            callNo, (int)name.size(), name.data(), r3, r4, r5, r6, r7, r8, result);
      }
    }
    return result;
  }
  SR_LOG_KERNEL_IMPORT("%sMISSING: %.*s (r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) -> stub returning %08X",
                       fromGuest ? "[GUEST] " : "",
                       (int)name.size(), name.data(), r3, r4, r5, r6, r7, r8, defaultVal);
  return defaultVal;
}

void ExportRegistry::listExports() const {
  std::lock_guard<std::mutex> lock(impl().mutex);
  SR_LOG_KERNEL_IMPORT("Registered exports (%zu):", impl().exports.size());
  for (const auto& [name, fn] : impl().exports) {
    (void)fn;
    SR_LOG_KERNEL_IMPORT("  %s", name.c_str());
  }
}

uint64_t ExportRegistry::getLastGuestCallTimeMs() {
  return g_lastGuestCallTimeMs.load(std::memory_order_relaxed);
}

bool ExportRegistry::hasGuestCallInLastMs(uint64_t ms) {
  uint64_t last = g_lastGuestCallTimeMs.load(std::memory_order_relaxed);
  if (last == 0) return false;  // no guest call yet
  uint64_t now = nowMs();
  return (now - last) <= ms;
}

uint32_t ExportRegistry::getGuestCallCount() {
  return g_guestCallCount.load(std::memory_order_relaxed);
}

void ExportRegistry::setCurrentExtraArgs(uint32_t r9, uint32_t r10) {
  g_guestArgR9 = r9;
  g_guestArgR10 = r10;
}

uint32_t ExportRegistry::getCurrentArgR9() {
  return g_guestArgR9;
}

uint32_t ExportRegistry::getCurrentArgR10() {
  return g_guestArgR10;
}

void ExportRegistry::setCurrentR13(uint32_t r13) {
  g_guestR13 = r13;
}

uint32_t ExportRegistry::getCurrentR13() {
  return g_guestR13;
}

}  // namespace kernel
}  // namespace sr
