// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST), before the suites that use it.
//
// A bare run of a gated suite. The project's suite runner and the post-merge check invoke every --*-test with no SR_*
// environment, but these suites' gates are latched at static initialisation (SR_NATIVE_FLUSH, SR_NATIVE_SEGMENTLESS, the
// suite's own gates) or on first use (the per-present A/B, SR_PM4_*), so they cannot be set from inside the process.
// A run is CONFIGURED when SR_AB_TOGGLE names the suite's own toggle exactly (its offline runner sets it in every
// mode). Otherwise the suite re-runs itself once as a child and returns the child's exit code (output on the same
// handles). The child's environment is built explicitly: the parent's entries with EVERY SR_* variable removed (except
// the two I6 attribution switches, passThrough below), then
// exactly the suite's configuration for the requested mode (as its runner builds it) plus SR_DIFFTEST_BARE_CHILD=<the
// suite argument> - a polluting variable in the caller cannot change what the bare run proves. Every step fails before
// spawning on error; a child that is not configured never launches again; the child checks its own SR_* environment
// and fails if anything outside the configuration is present (an ungraded line lists what it got).
#include <algorithm>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace sr::native::flush::bare {
using Conf = std::vector<std::pair<std::wstring, std::wstring>>;
constexpr const wchar_t* kMarker = L"SR_DIFFTEST_BARE_CHILD";

inline bool srName(const std::wstring& entry) {  // NAME=value: an SR_* name (Windows names are case-insensitive)
  return entry.size() >= 3 && (entry[0] == L'S' || entry[0] == L's') && (entry[1] == L'R' || entry[1] == L'r') &&
         entry[2] == L'_';
}
// The A/B and mutant gates of step 6 I6's Entry::Impl free list (kernel/native_flush.cpp): ATTRIBUTION switches, not a
// suite's configuration - the bare child keeps the caller's value, so `SR_ENTRY_POOL=0 difftest --<suite>` runs the
// heap path and SR_ENTRY_POOL_TEST_SKIP=<k> the mutant in the child too (the coordinator 2026-10-23: before this every
// bare suite ran the pool ON whatever the caller set). Step 6 I7's SR_SEG_ARENA / SR_SEG_ARENA_KEEP / SR_SEG_ARENA_HASH
// likewise (its =0 arm's capture checksums - opt-in, HASH=1 -, its ASan run's freed chunks), and step 6 I12's
// SR_AUDIT_RING (the reclaim audit's =0 arm, the map), step 6 I14b's SR_AUDIT_PREFETCH (=0: the reclaim walk's header
// prefetches off), and step 6 I15's SR_GUESTMEM_LEAN (the guest-memory bundle's =0 arm: before this the bare suites -
// native-bracket / native-residue / flush-split - ran the bundle ON in that arm), and step 6 R1a's SR_RENDER_KEEP_LEAN
// (=0: the render recording's per-slice / per-bind retention). Nothing else passes through.
inline bool passThrough(const std::wstring& entry) {
  const std::wstring name = entry.substr(0, entry.find(L'='));
  return _wcsicmp(name.c_str(), L"SR_ENTRY_POOL") == 0 || _wcsicmp(name.c_str(), L"SR_ENTRY_POOL_TEST_SKIP") == 0 ||
         _wcsicmp(name.c_str(), L"SR_SEG_ARENA") == 0 || _wcsicmp(name.c_str(), L"SR_SEG_ARENA_KEEP") == 0 ||
         _wcsicmp(name.c_str(), L"SR_SEG_ARENA_HASH") == 0 || _wcsicmp(name.c_str(), L"SR_AUDIT_RING") == 0 ||
         _wcsicmp(name.c_str(), L"SR_AUDIT_PREFETCH") == 0 || _wcsicmp(name.c_str(), L"SR_GUESTMEM_LEAN") == 0 ||
         _wcsicmp(name.c_str(), L"SR_RENDER_KEEP_LEAN") == 0;
}
inline int fail(const char* tag, const char* what) {
  std::printf("FAIL: %s\n[%s] 0 passed, 1 failed\n", what, tag);
  return 1;
}
// The value compared exactly (case-sensitive), as the A/B registry kernel/ab_toggle.cpp compares SR_AB_TOGGLE's names:
// a differently-cased toggle enables nothing there, so it must not count as configured here either.
inline bool envIs(const wchar_t* name, const wchar_t* value) {
  wchar_t buf[512];
  const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
  return n && n < sizeof(buf) / sizeof(buf[0]) && std::wcscmp(buf, value) == 0;
}
inline bool hasEnv(const wchar_t* name) {
  wchar_t buf[4];
  SetLastError(0);
  return GetEnvironmentVariableW(name, buf, 4u) != 0u || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

inline int relaunch(const char* tag, const char* arg, const char* mode, const Conf& conf) {
  wchar_t* const parent = GetEnvironmentStringsW();
  if (!parent) return fail(tag, "bare run: the parent environment could not be read");
  std::vector<std::wstring> vars;
  size_t dropped = 0;
  std::string kept;   // the I6 attribution switches passed through (passThrough)
  for (const wchar_t* e = parent; *e; e += std::wcslen(e) + 1) {
    std::wstring entry(e);
    if (srName(entry)) {
      if (passThrough(entry)) {
        for (wchar_t ch : entry + L" ") kept += char(ch);
        vars.push_back(std::move(entry));
        continue;
      }
      dropped++;
      continue;
    }
    vars.push_back(std::move(entry));
  }
  FreeEnvironmentStringsW(parent);
  std::wstring argW, modeW;
  for (const char* p = arg; *p; ++p) argW += wchar_t(*p);
  for (const char* p = mode; *p; ++p) modeW += wchar_t(*p);
  std::string shown;
  for (const auto& kv : conf) {
    if (kv.first.empty() || !srName(kv.first + L"=")) return fail(tag, "bare run: a configuration name is not SR_*");
    vars.push_back(kv.first + L"=" + kv.second);
    for (wchar_t ch : kv.first + L"=" + kv.second + L" ") shown += char(ch);
  }
  vars.push_back(std::wstring(kMarker) + L"=" + argW);
  std::sort(vars.begin(), vars.end(),
            [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  std::vector<wchar_t> block;
  for (const std::wstring& v : vars) {
    block.insert(block.end(), v.begin(), v.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  wchar_t exe[MAX_PATH * 2];
  const DWORD len = GetModuleFileNameW(nullptr, exe, DWORD(sizeof(exe) / sizeof(exe[0])));
  if (!len || len >= sizeof(exe) / sizeof(exe[0])) return fail(tag, "bare run: the difftest executable path");
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + argW + L" " + modeW;
  std::printf("[DIFFTEST_BARE] %s %s: not configured (SR_AB_TOGGLE): re-running in a child whose environment drops the "
              "caller's %zu SR_* variable(s) and sets %s%s%s\n", arg, mode, dropped, shown.c_str(),
              kept.empty() ? "" : " | passed through: ", kept.c_str());
  std::fflush(stdout);
  std::fflush(stderr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError})
    if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data(), nullptr, &si,
                      &pi)) {
    std::printf("FAIL: bare run: the configured child did not start (error %lu)\n[%s] 0 passed, 1 failed\n",
                GetLastError(), tag);
    return 1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0u) std::printf("[DIFFTEST_BARE] %s %s: the child exited with 0x%08lX\n", arg, mode, code);
  return int(code);  // the child's status as is - a crash (0xC0000005) is a negative int, still a failure
}

// The child: exactly the configuration's SR_* names, the marker, and difftest's own harness defaults SR_MODS=0 /
// SR_DEVTOOLS=0 (tools/difftest/difftest_main.cpp puts them into every run before any suite starts).
inline bool childOk(const char* tag, const Conf& conf) {
  wchar_t* const env = GetEnvironmentStringsW();
  if (!env) return fail(tag, "bare child: the environment could not be read") == 0;
  std::string names, extra;
  for (const wchar_t* e = env; *e; e += std::wcslen(e) + 1) {
    const std::wstring entry(e);
    if (!srName(entry)) continue;
    const std::wstring name = entry.substr(0, entry.find(L'='));
    bool ok = _wcsicmp(entry.c_str(), L"SR_MODS=0") == 0 || _wcsicmp(entry.c_str(), L"SR_DEVTOOLS=0") == 0 ||
              _wcsicmp(name.c_str(), kMarker) == 0 || passThrough(entry);
    for (const auto& kv : conf) ok = ok || _wcsicmp(kv.first.c_str(), name.c_str()) == 0;
    std::string& to = ok ? names : extra;
    for (wchar_t ch : entry) to += char(ch);
    to += ' ';
  }
  FreeEnvironmentStringsW(env);
  std::printf("[DIFFTEST_BARE] child SR_* environment: %s\n", names.c_str());
  if (!extra.empty()) {
    std::printf("FAIL: bare child: SR_* variables outside the suite's configuration: %s\n[%s] 0 passed, 1 failed\n",
                extra.c_str(), tag);
    return false;
  }
  return true;
}

// A deliberate child crash for the launcher's own test (difftest builds only; the variable is not SR_*, so the
// launcher passes it through): DIFFTEST_BARE_CRASH_CHILD=1 makes the configured child fault on a null write.
inline void crashChildForTest() {
  if (!hasEnv(kMarker)) return;
  wchar_t buf[4];
  if (GetEnvironmentVariableW(L"DIFFTEST_BARE_CRASH_CHILD", buf, 4u) == 1u && buf[0] == L'1') {
    std::printf("[DIFFTEST_BARE] DIFFTEST_BARE_CRASH_CHILD=1: the child faults now\n");
    std::fflush(stdout);
    *static_cast<volatile int*>(nullptr) = 0;
  }
}

// The suite's entry. Returns true when this process is done and must return `code` (the child's exit status, a
// launcher failure, or a child-environment failure); false when it is the configured run and goes on. Whether to go
// on never depends on the value of the child's status (a crash's NTSTATUS is a negative int).
inline bool handled(const char* tag, const char* arg, const char* mode, const wchar_t* toggle, const Conf& conf,
                    int& code) {
  if (!envIs(L"SR_AB_TOGGLE", toggle)) {
    code = hasEnv(kMarker) ? fail(tag, "bare run: the child is not configured (no second launch)")
                           : relaunch(tag, arg, mode, conf);
    return true;
  }
  if (hasEnv(kMarker)) {
    if (!childOk(tag, conf)) {
      code = 1;
      return true;
    }
    crashChildForTest();
  }
  return false;
}
}  // namespace sr::native::flush::bare
