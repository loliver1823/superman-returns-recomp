#pragma once

#include <windows.h>
#include <psapi.h>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sr::native::memory {

inline bool writableProtection(DWORD protection) noexcept {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  switch (protection & 0xFFu) {
    case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
  }
}

inline bool writableVirtualRange(const void* pointer, size_t bytes) noexcept {
  const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
  if (!begin || !bytes || bytes > std::numeric_limits<uintptr_t>::max() - begin) return false;
  const uintptr_t end = begin + bytes;
  for (uintptr_t at = begin; at < end;) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || !writableProtection(region.Protect)) return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
}

using WorkingSetQuery = BOOL (WINAPI*)(HANDLE, PVOID, DWORD);

// Fresh permission snapshot, never a permission cache. The caller owns mapping
// and protection lifetimes through the subsequent writes, as with VirtualQuery.
// The query parameter permits difftest to exercise actual OS-query failure.
inline bool writableRange(const void* pointer, size_t bytes,
                          WorkingSetQuery query = K32QueryWorkingSetEx) noexcept {
  const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
  if (!begin || !bytes || bytes > std::numeric_limits<uintptr_t>::max() - begin) return false;
  static const uintptr_t pageSize = [] {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return uintptr_t(info.dwPageSize);
  }();
  if (!pageSize) return writableVirtualRange(pointer, bytes);
  const uintptr_t firstPage = begin - begin % pageSize;
  const size_t count = (begin + bytes - 1u - firstPage) / pageSize + 1u;
  PSAPI_WORKING_SET_EX_INFORMATION pages[16]{};
  if (!query || count > 16u) return writableVirtualRange(pointer, bytes);
  for (size_t i = 0; i < count; ++i)
    pages[i].VirtualAddress = reinterpret_cast<void*>(firstPage + i * pageSize);
  if (!query(GetCurrentProcess(), pages, DWORD(count * sizeof(pages[0]))))
    return writableVirtualRange(pointer, bytes);
  for (size_t i = 0; i < count; ++i) {
    const auto attributes = pages[i].VirtualAttributes;
    // Invalid pages have no meaningful Win32Protection: demand-zero / evicted
    // pages must use the complete VirtualQuery walk, including MEM_COMMIT.
    if (!attributes.Valid) return writableVirtualRange(pointer, bytes);
    if (attributes.Bad || !writableProtection(DWORD(attributes.Win32Protection))) return false;
  }
  return true;
}

}  // namespace sr::native::memory
