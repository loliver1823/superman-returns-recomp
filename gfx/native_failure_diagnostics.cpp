#include "native_failure_diagnostics.hpp"
#include "app/log.hpp"
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#include <cstdio>

namespace sr::gfx {
namespace {
using Microsoft::WRL::ComPtr;
using U64 = unsigned long long;

void reportHostMemory() noexcept {
  PROCESS_MEMORY_COUNTERS_EX process{};
  process.cb = sizeof(process);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process))) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_GPU_FAILURE] process known=1 private_bytes=%llu working_set=%llu peak_working_set=%llu",
                U64(process.PrivateUsage), U64(process.WorkingSetSize), U64(process.PeakWorkingSetSize));
  } else {
    const DWORD error = GetLastError();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] process known=0 error=%lu", error);
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_GPU_FAILURE] physical known=1 total=%llu available=%llu process_commit_available=%llu",
                U64(memory.ullTotalPhys), U64(memory.ullAvailPhys), U64(memory.ullAvailPageFile));
  } else {
    const DWORD error = GetLastError();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] physical known=0 error=%lu", error);
  }
  PERFORMANCE_INFORMATION performance{};
  performance.cb = sizeof(performance);
  if (GetPerformanceInfo(&performance, sizeof(performance))) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_GPU_FAILURE] system_commit known=1 total_pages=%llu limit_pages=%llu peak_pages=%llu page_bytes=%llu",
                U64(performance.CommitTotal), U64(performance.CommitLimit),
                U64(performance.CommitPeak), U64(performance.PageSize));
  } else {
    const DWORD error = GetLastError();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] system_commit known=0 error=%lu", error);
  }
  BOOL inJob = FALSE;
  if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob)) {
    const DWORD error = GetLastError();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] job known=0 error=%lu", error);
  } else if (!inJob) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] job known=1 member=0");
  } else {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr)) {
      // nullptr reports the current job, not every ancestor of a nested job.
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[NATIVE_GPU_FAILURE] job known=1 member=1 scope=current flags=0x%08lX process_cap_active=%u process_cap=%llu job_cap_active=%u job_cap=%llu peak_process=%llu peak_job=%llu",
                  limits.BasicLimitInformation.LimitFlags,
                  unsigned((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0),
                  U64(limits.ProcessMemoryLimit),
                  unsigned((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0),
                  U64(limits.JobMemoryLimit), U64(limits.PeakProcessMemoryUsed), U64(limits.PeakJobMemoryUsed));
    } else {
      const DWORD error = GetLastError();
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] job known=0 member=1 error=%lu", error);
    }
  }
}

void reportAdapterMemory(ID3D12Device* device, IDXGIFactory4* factory) noexcept {
  if (!device || !factory) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] adapter known=0 device=%u factory=%u",
                unsigned(device != nullptr), unsigned(factory != nullptr));
    return;
  }
  const LUID luid = device->GetAdapterLuid();
  ComPtr<IDXGIAdapter3> adapter;
  const HRESULT found = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
  if (FAILED(found)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] adapter known=0 hr=0x%08X luid=%08lX:%08lX",
                unsigned(found), DWORD(luid.HighPart), luid.LowPart);
    return;
  }
  for (unsigned group = 0; group < 2; ++group) {
    DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
    const auto segment = group == 0 ? DXGI_MEMORY_SEGMENT_GROUP_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL;
    const HRESULT queried = adapter->QueryVideoMemoryInfo(0, segment, &budget);
    if (SUCCEEDED(queried)) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[NATIVE_GPU_FAILURE] adapter known=1 node=0 segment=%s luid=%08lX:%08lX budget=%llu usage=%llu reservation=%llu available_for_reservation=%llu",
                  group == 0 ? "local" : "nonlocal", DWORD(luid.HighPart), luid.LowPart,
                  U64(budget.Budget), U64(budget.CurrentUsage), U64(budget.CurrentReservation),
                  U64(budget.AvailableForReservation));
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] adapter known=0 node=0 segment=%s hr=0x%08X",
                  group == 0 ? "local" : "nonlocal", unsigned(queried));
    }
  }
}

void reportDred(ID3D12Device* device) noexcept {
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  const HRESULT available = device->QueryInterface(IID_PPV_ARGS(&dred));
  if (FAILED(available)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred known=0 hr=0x%08X", unsigned(available));
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
  const HRESULT breadcrumbResult = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_breadcrumbs hr=0x%08X limit=8",
              unsigned(breadcrumbResult));
  if (SUCCEEDED(breadcrumbResult)) {
    const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode;
    unsigned count = 0;
    for (; node && count < 8; node = node->pNext, ++count) {
      const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[NATIVE_GPU_FAILURE] dred_list index=%u completed_known=%u done=%u total=%u list=%.96s queue=%.96s next_op_known=%u next_op=%u",
                  count, unsigned(node->pLastBreadcrumbValue != nullptr), done, node->BreadcrumbCount,
                  node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                  node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?",
                  unsigned(node->pLastBreadcrumbValue && node->pCommandHistory && done < node->BreadcrumbCount),
                  node->pLastBreadcrumbValue && node->pCommandHistory && done < node->BreadcrumbCount
                      ? unsigned(node->pCommandHistory[done]) : 0u);
    }
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_lists count=%u truncated=%u", count, unsigned(node != nullptr));
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
  const HRESULT faultResult = dred->GetPageFaultAllocationOutput(&fault);
  if (FAILED(faultResult)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_fault known=0 hr=0x%08X", unsigned(faultResult));
    return;
  }
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_fault known=1 va=0x%016llX", U64(fault.PageFaultVA));
  for (unsigned freed = 0; freed < 2; ++freed) {
    const auto* node = freed ? fault.pHeadRecentFreedAllocationNode : fault.pHeadExistingAllocationNode;
    unsigned count = 0;
    for (; node && count < 4; node = node->pNext, ++count)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_allocation freed=%u index=%u name=%.96s type=%u",
                  freed, count, node->ObjectNameA ? node->ObjectNameA : "?", unsigned(node->AllocationType));
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_GPU_FAILURE] dred_allocations freed=%u count=%u truncated=%u",
                freed, count, unsigned(node != nullptr));
  }
}
}  // namespace

void reportNativeGpuFailure(ID3D12Device* device, IDXGIFactory4* factory,
                            const char* where, HRESULT original) noexcept {
  sr::Log::EmergencyScope emergency;
  const HRESULT removed = device ? device->GetDeviceRemovedReason() : E_POINTER;
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[NATIVE_GPU_FAILURE] where=%.96s original_hr=0x%08X device_known=%u removed_reason=0x%08X",
              where ? where : "?", unsigned(original), unsigned(device != nullptr), unsigned(removed));
  reportHostMemory();
  reportAdapterMemory(device, factory);
  if (device && FAILED(removed)) reportDred(device);
  const bool flushed = sr::Log::flush(1000);
  if (!flushed) {
    // The caller may immediately _Exit. Keep loss explicit even if the logger's
    // writer is unavailable; never wait indefinitely or re-enter the renderer.
    std::fprintf(stderr, "[NATIVE_GPU_FAILURE] log_flush=0 emergency_lost=%u\n", emergency.failed());
    std::fflush(stderr);
  }
}
}  // namespace sr::gfx
