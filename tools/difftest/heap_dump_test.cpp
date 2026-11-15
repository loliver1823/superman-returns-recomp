// step4/relcrash (lane G, 2026-10-18): --heap-dump-test - the crash handler's heap dump (SR_CRASH_DUMP_HEAP) and
// the LFH UserBlocks sentinel (SR_HEAP_SENTINEL), the two instruments for the s4h_wg_rel1 heap clobber.
//
// The release whole-game run died in RtlFreeHeap on a clobbered LFH UserBlocks header and its minidump held no heap
// bytes (archive/step4_claude_20261018/relcrash/NOTES.md). These checks pin the instruments on this process:
//   H1  writeDiagnosticMinidump(path, withHeap=true) writes a dump whose raw bytes hold a 64-byte marker that lives
//       only in a heap block, and NOT a second marker that lives only inside the excluded guest window (a top-down
//       VirtualAlloc region set as g_debugGuestBase for the call)
//   H2  mutation witness: the default dump (withHeap=false) does NOT hold the heap marker - the option is what adds
//       heap bytes (a heap dump that silently fell back to the default would fail H1)
//   H3  non-vacuity of H1's exclusion: with g_debugGuestBase unset the window marker IS in the heap dump
//   S1  the sentinel's self-test (a private LFH heap, one UserBlocks header clobbered with the rel1 pattern) PASSES
//   S2  mutation witness: with the plausibility check disabled (mutation 1) the same self-test FAILS
// The markers are written straight into their blocks (never built on the stack: thread stacks are in every dump);
// the search compares against a bit-inverted copy kept elsewhere.
#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/crash_handler.hpp"
#include "app/heap_sentinel.hpp"

extern "C" uint8_t* g_debugGuestBase;

namespace {

constexpr size_t kMarker = 64;

void fillMarker(uint8_t* dst, uint8_t* inverted, uint64_t seed) {
  for (size_t i = 0; i < kMarker; ++i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    const uint8_t b = uint8_t(seed >> 56);
    dst[i] = b;
    inverted[i] = uint8_t(~b);
  }
}

// Does the file hold the marker whose bit-inverted copy is `inverted`?
bool fileHolds(const std::string& path, const uint8_t* inverted) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> want(kMarker);
  for (size_t i = 0; i < kMarker; ++i) want[i] = uint8_t(~inverted[i]);
  std::vector<uint8_t> buf(1 << 20);
  size_t carry = 0;
  bool hit = false;
  for (;;) {
    const size_t n = std::fread(buf.data() + carry, 1, buf.size() - carry, f);
    const size_t have = carry + n;
    if (have >= kMarker)
      for (size_t i = 0; i + kMarker <= have; ++i)
        if (buf[i] == want[0] && std::memcmp(buf.data() + i, want.data(), kMarker) == 0) { hit = true; break; }
    if (hit || n == 0) break;
    carry = (std::min)(have, kMarker - 1);
    std::memmove(buf.data(), buf.data() + have - carry, carry);
  }
  std::fclose(f);
  // the expected pattern now sits in `want` (heap) as well: it is never in a dump written BEFORE this call
  return hit;
}

uint64_t fileBytes(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return 0;
  return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

}  // namespace

int runHeapDumpTest() {
  std::printf("heap_dump_test (step4/relcrash: SR_CRASH_DUMP_HEAP + SR_HEAP_SENTINEL)\n");
  CreateDirectoryA("archive", nullptr);
  const std::string heapDump = "archive/difftest_heap_dump.dmp";
  const std::string plainDump = "archive/difftest_plain_dump.dmp";
  const std::string openDump = "archive/difftest_open_dump.dmp";

  // The heap marker (a heap block) and the window marker (a region far from the heaps: top-down).
  auto* heapBlock = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, 4096));
  auto* heapInv = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, kMarker));
  auto* win = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
  auto* winInv = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, kMarker));
  if (!heapBlock || !heapInv || !win || !winInv) {
    std::printf("  FAIL allocation\n");
    return 1;
  }
  fillMarker(heapBlock + 1024, heapInv, GetTickCount64() ^ 0x5EEDull);
  fillMarker(win + 4096, winInv, GetTickCount64() ^ 0xA11Cull);

  uint8_t* const savedBase = g_debugGuestBase;
  g_debugGuestBase = win;   // the window [win - 4 GB, win + 4 GB) is excluded from the heap walk
  const bool w1 = sr::writeDiagnosticMinidump(heapDump.c_str(), /*withHeap=*/true);
  g_debugGuestBase = savedBase;
  const bool w2 = sr::writeDiagnosticMinidump(plainDump.c_str(), /*withHeap=*/false);
  g_debugGuestBase = nullptr;  // no window: the region is walked like any private region
  const bool w3 = sr::writeDiagnosticMinidump(openDump.c_str(), /*withHeap=*/true);
  g_debugGuestBase = savedBase;

  std::printf("  dumps: heap %llu B, plain %llu B, unwindowed %llu B\n", (unsigned long long)fileBytes(heapDump),
              (unsigned long long)fileBytes(plainDump), (unsigned long long)fileBytes(openDump));
  check(w1 && w2 && w3, "the three dumps were written");
  check(fileHolds(heapDump, heapInv), "H1 the heap dump holds the heap block's marker");
  check(!fileHolds(heapDump, winInv), "H1 the heap dump does not hold the marker inside the excluded guest window");
  check(!fileHolds(plainDump, heapInv), "H2 (mutation witness) the default dump does not hold the heap marker");
  check(fileHolds(openDump, winInv), "H3 (non-vacuity) with no guest window set, the region's marker is in the dump");

  check(sr::heapSentinelSelfTestForTest(0), "S1 the UserBlocks sentinel reports a clobbered header (self-test PASS)");
  check(!sr::heapSentinelSelfTestForTest(1),
        "S2 (mutation witness) with its plausibility check disabled the same self-test FAILS");

  DeleteFileA(heapDump.c_str());
  DeleteFileA(plainDump.c_str());
  DeleteFileA(openDump.c_str());
  VirtualFree(win, 0, MEM_RELEASE);
  HeapFree(GetProcessHeap(), 0, heapBlock);
  HeapFree(GetProcessHeap(), 0, heapInv);
  HeapFree(GetProcessHeap(), 0, winInv);
  std::printf("[HEAP_DUMP_TEST] %d failed\n", g_fail);
  return g_fail ? 1 : 0;
}
#else
#include <cstdio>
int runHeapDumpTest() {
  std::printf("[HEAP_DUMP_TEST] Windows only\n");
  return 0;
}
#endif
