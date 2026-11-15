// Fresh native SDK output-permission admission, real Windows protection changes.
// build/difftest.exe --native-memory-test: no game, rendering or Native0 proof.
#include "kernel/native_memory.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {
namespace M = sr::native::memory;
int passed = 0, failed = 0;
unsigned queries = 0, queriedPages = 0;
uintptr_t firstQuery = 0, lastQuery = 0;
void check(bool condition, const char* description) {
  std::printf("  %s %s\n", condition ? "ok " : "FAIL:", description);
  condition ? ++passed : ++failed;
}
BOOL WINAPI observeQuery(HANDLE process, PVOID buffer, DWORD bytes) {
  ++queries;
  auto* pages = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer);
  queriedPages = bytes / sizeof(pages[0]);
  firstQuery = reinterpret_cast<uintptr_t>(pages[0].VirtualAddress);
  lastQuery = reinterpret_cast<uintptr_t>(pages[queriedPages - 1u].VirtualAddress);
  return K32QueryWorkingSetEx(process, buffer, bytes);
}
BOOL WINAPI invalidQuery(HANDLE, PVOID buffer, DWORD bytes) {
  ++queries;
  auto* pages = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer);
  for (size_t i = 0; i < bytes / sizeof(pages[0]); ++i) {
    pages[i].VirtualAttributes.Flags = 0;
    pages[i].VirtualAttributes.Win32Protection = PAGE_READWRITE;
  }
  return TRUE;
}
BOOL WINAPI failedQuery(HANDLE process, PVOID buffer, DWORD bytes) {
  invalidQuery(process, buffer, bytes);
  auto* pages = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer);
  for (size_t i = 0; i < bytes / sizeof(pages[0]); ++i) pages[i].VirtualAttributes.Valid = 1;
  return FALSE;
}
BOOL WINAPI badPageQuery(HANDLE process, PVOID buffer, DWORD bytes) {
  failedQuery(process, buffer, bytes);
  auto* pages = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer);
  for (size_t i = 0; i < bytes / sizeof(pages[0]); ++i) pages[i].VirtualAttributes.Bad = 1;
  return TRUE;
}
struct Allocation {
  uint8_t* pointer;
  ~Allocation() { if (pointer) VirtualFree(pointer, 0, MEM_RELEASE); }
  bool release() {
    if (!pointer || !VirtualFree(pointer, 0, MEM_RELEASE)) return false;
    pointer = nullptr;
    return true;
  }
};
}

int runNativeMemoryTest() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const size_t pageSize = info.dwPageSize;
  Allocation pages{static_cast<uint8_t*>(VirtualAlloc(nullptr, 18u * pageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
  Allocation cold{static_cast<uint8_t*>(VirtualAlloc(nullptr, 4u * pageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
  Allocation reserved{static_cast<uint8_t*>(VirtualAlloc(nullptr, 4u * pageSize, MEM_RESERVE, PAGE_READWRITE))};
  check(pageSize && pages.pointer && cold.pointer && reserved.pointer, "bounded page-aligned fixtures allocate");
  if (!pageSize || !pages.pointer || !cold.pointer || !reserved.pointer) return 1;
  std::memset(pages.pointer, 0x5A, 18u * pageSize); // resident positive query path
  queries = 0;
  check(M::writableRange(pages.pointer + pageSize - 7u, 2u * pageSize + 14u, observeQuery) && queries == 1u &&
        queriedPages == 4u && firstQuery == reinterpret_cast<uintptr_t>(pages.pointer) &&
        lastQuery == reinterpret_cast<uintptr_t>(pages.pointer + 3u * pageSize),
        "one fresh batch covers partial first/last and every interior page");
  queries = 0;
  check(M::writableRange(pages.pointer, 16u * pageSize, observeQuery) && queries == 1u && queriedPages == 16u,
        "fixed-capacity resident range uses all sixteen entries");
  queries = 0;
  check(M::writableRange(pages.pointer, 17u * pageSize, observeQuery) && queries == 0u,
        "oversized range uses complete VirtualQuery fallback without overflowing batch");
  PSAPI_WORKING_SET_EX_INFORMATION coldPage{};
  coldPage.VirtualAddress = cold.pointer;
  check(K32QueryWorkingSetEx(GetCurrentProcess(), &coldPage, sizeof(coldPage)) && !coldPage.VirtualAttributes.Valid &&
        M::writableRange(cold.pointer, 4u * pageSize), "real nonresident committed pages accept through fallback");
  check(!M::writableRange(reserved.pointer, 4u * pageSize), "reserved uncommitted range rejects");
  check(!M::writableRange(pages.pointer, 4u * pageSize, badPageQuery), "valid query reporting a bad writable page rejects");
  for (DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
    for (size_t page : {0u, 1u, 3u}) {
      DWORD old = 0, ignored = 0;
      const bool changed = VirtualProtect(pages.pointer + page * pageSize, pageSize, protection, &old) != 0;
      const bool handled = changed && M::writableRange(pages.pointer + pageSize - 7u, 2u * pageSize + 14u);
      MEMORY_BASIC_INFORMATION after{};
      const bool unchanged = VirtualQuery(pages.pointer + page * pageSize, &after, sizeof(after)) && after.Protect == protection;
      const bool restored = changed && VirtualProtect(pages.pointer + page * pageSize, pageSize, old, &ignored);
      char label[160];
      std::snprintf(label, sizeof(label), "mixed protected page%zu protection%X rejects, guard/protection unchanged, restore succeeds", page, unsigned(protection));
      check(changed && !handled && unchanged && restored, label);
      if (!restored) return 1;
      check(restored && M::writableRange(pages.pointer, 4u * pageSize), "RW to protected to RW sees fresh permissions");
    }
  }
  DWORD old = 0, ignored = 0;
  const bool changed = VirtualProtect(pages.pointer + pageSize, pageSize, PAGE_READONLY, &old) != 0;
  check(changed && !M::writableRange(pages.pointer, 4u * pageSize, invalidQuery),
        "successful query with invalid writable-looking fields uses complete fallback");
  check(changed && !M::writableRange(pages.pointer, 4u * pageSize, failedQuery),
        "failed query with valid writable-looking fields uses complete fallback");
  check(changed && !M::writableRange(pages.pointer, 4u * pageSize, nullptr), "unavailable query uses complete fallback");
  check(changed && !M::writableRange(pages.pointer, 17u * pageSize), "oversized fallback rejects protected interior");
  const bool restored = changed && VirtualProtect(pages.pointer + pageSize, pageSize, old, &ignored);
  check(restored, "fallback fixture protection restores");
  if (!restored) return 1;
  check(M::writableRange(pages.pointer, 4u * pageSize, invalidQuery) &&
        M::writableRange(pages.pointer, 4u * pageSize, failedQuery) && M::writableRange(pages.pointer, 4u * pageSize, nullptr),
        "all fallback forms accept committed writable range");
  check(VirtualFree(pages.pointer + pageSize, pageSize, MEM_DECOMMIT) && !M::writableRange(pages.pointer, 4u * pageSize),
        "uncommitted interior rejects complete range");
  const bool recommitted = VirtualAlloc(pages.pointer + pageSize, pageSize, MEM_COMMIT, PAGE_READWRITE) == pages.pointer + pageSize;
  check(recommitted && M::writableRange(pages.pointer, 4u * pageSize), "recommitted interior accepts fresh range");
  if (!recommitted) return 1;
  queries = 0;
  check(!M::writableRange(nullptr, 1u, observeQuery) && !M::writableRange(pages.pointer, 0u, observeQuery) &&
        !M::writableRange(reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 2u), 4u, observeQuery) &&
        queries == 0u, "null, empty and overflowing ranges reject before query");
  // Diagnostic only: same live fixture and order-alternated batches. No speed gate
  // or FPS inference; actual SDK admission cost is measured in host profiles.
  std::memset(pages.pointer, 0x5A, 18u * pageSize);
  using Clock = std::chrono::steady_clock;
  double fastMs = 0, slowMs = 0;
  bool allWritable = true;
  for (unsigned lap = 0; lap < 6u; ++lap) for (unsigned arm = 0; arm < 2u; ++arm) {
    const bool fast = (arm ^ (lap & 1u)) == 0u;
    const auto start = Clock::now();
    for (unsigned i = 0; i < 2000u; ++i)
      allWritable &= fast ? M::writableRange(pages.pointer, 5u * pageSize) : M::writableVirtualRange(pages.pointer, 5u * pageSize);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    (fast ? fastMs : slowMs) += ms;
  }
  std::printf("[NATIVE_MEMORY_BENCH] same fixture12000 calls/arm working-set %.3fms virtual-query %.3fms; diagnostic only\n", fastMs, slowMs);
  check(allWritable, "bounded repeated permission queries retain writable result");
  check(pages.release() && cold.release() && reserved.release(), "all fixture allocations release");
  std::printf("[NATIVE_MEMORY_TEST] %d passed, %d failed (host permission admission only)\n", passed, failed);
  return failed ? 1 : 0;
}
