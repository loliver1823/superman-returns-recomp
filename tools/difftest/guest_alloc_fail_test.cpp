// step4 stuck-load lane (2026-10-20): [GUEST_ALLOC_FAIL] (kernel/guest_alloc_fail.hpp) - the rate rule, the PLAYER
// log profile keeping every rate-limited line (the per-site Warn cap of 4 must not apply), and the REAL exports
// reporting their failures with the return values they always had (log-only).
//
// Run: build/difftest.exe --guest-alloc-fail-test   (exit code = failed checks)
#include "kernel/export_registry.hpp"
#include "kernel/guest_alloc_fail.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "app/log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <process.h>

namespace K = sr::kernel;
using Api = sr::kernel::GuestAllocApi;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
  std::fflush(stdout);
}
uint32_t be32(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  return p ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] : 0xDEADBEEFu;
}
void putBe32(uint32_t va, uint32_t v) {
  uint8_t* p = K::guestVAToHost(va);
  if (p) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
}
size_t countLines(const std::string& text, const std::string& needle) {
  size_t n = 0;
  std::istringstream in(text);
  for (std::string ln; std::getline(in, ln);)
    if (ln.find("[GUEST_ALLOC_FAIL]") != std::string::npos && ln.find(needle) != std::string::npos) ++n;
  return n;
}
}  // namespace

int runGuestAllocFailTest() {
  std::printf("[GUEST_ALLOC_FAIL_TEST] the guest allocation-failure breadcrumb\n");
  g_pass = g_fail = 0;

  // ── G1: the rate rule - failures 1-16, then every power of two ──────────────────────────────────────────────
  {
    bool first16 = true, gap = true;
    for (uint32_t n = 1; n <= 16; ++n) first16 &= K::guestAllocFailLogged(n);
    for (uint32_t n = 17; n <= 31; ++n) gap &= !K::guestAllocFailLogged(n);
    check(first16, "G1a failures #1-16 are logged");
    check(gap, "G1b #17-31 are not");
    check(K::guestAllocFailLogged(32) && K::guestAllocFailLogged(64) && K::guestAllocFailLogged(1u << 31) &&
              !K::guestAllocFailLogged(33) && !K::guestAllocFailLogged(96) && !K::guestAllocFailLogged(0),
          "G1c then every power of two (#32, #64, ... #2^31), nothing else, never #0");
  }

  // ── G2: the PLAYER profile keeps every rate-limited line through the real Log (one call site, 40 failures) ─────
  namespace fs = std::filesystem;
  const fs::path logPath = fs::temp_directory_path() / ("sr_guest_alloc_fail_" + std::to_string(_getpid()) + ".log");
  _putenv_s("SR_LOG", "player");
  sr::Log::init(logPath.string().c_str());
  check(sr::Log::playerProfile(), "G2a the log runs the player profile");
  K::resetGuestAllocFailForTest();
  uint32_t logged = 0;
  for (int i = 0; i < 40; ++i)
    logged += K::noteGuestAllocFailure(Api::XamAlloc, 0x1000u + i, 0x20000000u, 0, "test") ? 1 : 0;
  check(logged == 17 && K::guestAllocFailCount(Api::XamAlloc) == 40,
        "G2b 40 failures: 17 written (#1-16, #32), the count is 40");
  check(!K::noteGuestAllocFailure(static_cast<Api>(99), 1, 0, 0, "bad"), "G2c an unknown API is refused");

  // ── G3: the REAL exports, unchanged return values, one report each ────────────────────────────────────────────
  K::registerStubs();
  auto& reg = K::ExportRegistry::instance();
  auto rtl = reg.getExport("RtlAllocateHeap");
  auto xam = reg.getExport("XamAlloc");
  auto ntavm = reg.getExport("NtAllocateVirtualMemory");
  auto mm = reg.getExport("MmAllocatePhysicalMemoryEx");
  check(rtl && xam && ntavm && mm, "G3a the four exports are registered");
  if (rtl && xam && ntavm && mm) {
    K::resetGuestAllocFailForTest();
    // RtlAllocateHeap(heap, flags, size): a 4 GB - 4 KB request cannot be served
    const uint32_t r = rtl(0x40000000u, 0x8u, 0xFFFFF000u, 0, 0, 0);
    check(r == 0 && K::guestAllocFailCount(Api::RtlAllocateHeap) == 1,
          "G3b RtlAllocateHeap: 0 returned as before, one failure reported");
    // XamAlloc(flags, size, out): E_OUTOFMEMORY and *out = 0, as before
    constexpr uint32_t kOut = 0x00F00000u;
    putBe32(kOut, 0x12345678u);
    const uint32_t x = xam(0x20000000u, 0xFFFF0000u, kOut, 0, 0, 0);
    check(x == 0x8007000Eu && be32(kOut) == 0 && K::guestAllocFailCount(Api::XamAlloc) == 1,
          "G3c XamAlloc: E_OUTOFMEMORY and *out = 0 as before, one failure reported");
    // NtAllocateVirtualMemory(&base = 0, &size = 3.75 GB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    constexpr uint32_t kBase = 0x00F00010u, kSize = 0x00F00014u;
    putBe32(kBase, 0);
    putBe32(kSize, 0xF0000000u);
    const uint32_t st = ntavm(kBase, kSize, 0x00003000u, 0x04u, 0, 0);
    check(st == 0xC0000017u && K::guestAllocFailCount(Api::NtAllocateVirtualMemory) == 1,
          "G3d NtAllocateVirtualMemory: STATUS_NO_MEMORY as before, one failure reported");
    // MmAllocatePhysicalMemoryEx: 496 MB grants until the physical region is exhausted (at most 4 calls)
    uint32_t got = 1;
    int calls = 0;
    while (got != 0 && calls < 4) { got = mm(0, 0x1F000000u, 4u, 0u, 0xFFFFFFFFu, 0u); ++calls; }
    check(got == 0 && K::guestAllocFailCount(Api::MmAllocatePhysicalMemoryEx) == 1,
          "G3e MmAllocatePhysicalMemoryEx: 0 when the region is exhausted, one failure reported");
  }
  sr::Log::shutdown();
  _putenv_s("SR_LOG", "");

  std::string text;
  {
    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  check(countLines(text, " XamAlloc size=0x10") == 17, "G4a the player log holds all 17 rate-limited G2 lines");
  check(countLines(text, "#32 XamAlloc size=0x101F flags=0x20000000 prot=0x00000000 lr=") == 1,
        "G4b #32 carries its running count, size, flags and prot");
  check(countLines(text, " tid=") >= 17 && countLines(text, " thread=") >= 17 && countLines(text, " handle=") >= 17,
        "G4c every line names the host tid, the guest thread label and its handle");
  check(countLines(text, "RtlAllocateHeap size=0xFFFFF000 flags=0x00000008") == 1 &&
            countLines(text, "XamAlloc size=0xFFFF0000 flags=0x20000000 prot=0x00000000") == 1 &&
            countLines(text, "NtAllocateVirtualMemory size=0xF0000000 flags=0x00003000 prot=0x00000004") == 1 &&
            countLines(text, "MmAllocatePhysicalMemoryEx size=0x1F000000 flags=0x00000000 prot=0x00000004") == 1,
        "G4d each real export's failure is in the player log with its own arguments");
  std::error_code ec;
  fs::remove(logPath, ec);   // this test's own temp file

  std::printf("[GUEST_ALLOC_FAIL_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
