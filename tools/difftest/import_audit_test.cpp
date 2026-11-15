// step4/audit: --import-audit-test - the SR_AUDIT_IMPORTS census (kernel/import_audit.cpp) and its wiring in the
// import glue (guest/ppc_import_glue.cpp callKernel). Only an audit build tree has it (cmake -DSR_AUDIT_IMPORTS=ON);
// any other build answers "not compiled" with exit 2.
//   A1 direct calls: 20 distinct caller lrs -> 16 bins + lr_other 4; results 0..9 twice -> 8 bins + results_other 4;
//      the first 8 calls kept with their arguments and results
//   A2 calls with no registered export count as unresolved
//   A3 the REAL callKernel with a registered test export: one entry, one return, its lr, first argument, result
//   A4 the REAL callKernel with no export: unresolved 1, result 0
//   A5 an entry without a return (a call that never returns): entries 1, returns 0, first result null
//   A6 the REAL callKernel with a FAULTING export (its __except arm): entries 1, returns 0
//   A7 the REAL callKernel with a BLOCKED export, read while it blocks (entries 1, returns 0) and after (1 / 1)
//   A8 the final flush: a child difftest makes one call and leaves through std::quick_exit (the project's normal
//      exit), another through exit(); each child's census file holds the call, marked final
//   A9 PATHS (2026-10-20): NtCreateFile / NtOpenFile (OA r5) and NtQueryFullAttributesFile (OA r3) each get a record
//      in call order with lr, r4, the guest path (JSON-escaped; a handle-relative name tagged "<root ...>") and the
//      result; a call without an OA and another export get none
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/import_audit.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <windows.h>

#if SR_AUDIT_IMPORTS
uint32_t srTestAuditCallKernel(const char* name, uint32_t lr, uint32_t r3);   // guest/ppc_import_glue.cpp

namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  ok ? ++g_pass : ++g_fail;
}

std::string readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string recordOf(const std::string& json, const char* name) {
  const std::string key = std::string("{\"name\": \"") + name + "\"";
  const size_t a = json.find(key);
  if (a == std::string::npos) return {};
  const size_t e = json.find('\n', a);
  return json.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
size_t count(const std::string& s, const std::string& sub) {
  size_t n = 0;
  for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) ++n;
  return n;
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

uint32_t plusOne(uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return r3 + 1; }
uint32_t faulting(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  RaiseException(0xE0A0D17Fu, 0, 0, nullptr);   // caught by callKernel's __except arm
  return 1;
}
HANDLE g_release = nullptr;
uint32_t blocking(uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  WaitForSingleObject(g_release, 30000);
  return r3;
}

std::string snapshot(const std::string& path) {
  sr::audit::importAuditWriteJson(path.c_str());
  return readAll(path);
}

// A8's child process: one call, then the given exit. Its census file must still hold the call.
int child(const char* how) {
  sr::audit::importCall("SrAuditChild", 0x82345678u, 5, 0, 0, 0, 0, 0, 6, true);
  if (std::strcmp(how, "quick") == 0) std::quick_exit(0);
  std::exit(0);
}

bool runChild(const std::string& how, const std::string& out) {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  _putenv_s("SR_AUDIT_TEST_CHILD", how.c_str());
  _putenv_s("SR_AUDIT_IMPORTS_OUT", out.c_str());
  DeleteFileA(out.c_str());
  std::string cmd = std::string("\"") + exe + "\" --import-audit-test";
  STARTUPINFOA si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &pi);
  _putenv_s("SR_AUDIT_TEST_CHILD", "");
  if (!ok) return false;
  const bool done = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return done;
}

}  // namespace
#endif

int runImportAuditTest() {
#if !SR_AUDIT_IMPORTS
  std::printf("[IMPORT_AUDIT_TEST] not compiled: SR_AUDIT_IMPORTS is OFF in this build (an audit build tree only)\n");
  return 2;
#else
  if (const char* c = std::getenv("SR_AUDIT_TEST_CHILD"); c && c[0]) return child(c);
  namespace K = sr::kernel;
  namespace A = sr::audit;
  const char* tmp = std::getenv("TEMP");
  const std::string dir = tmp ? std::string(tmp) : std::string(".");
  const std::string bg = dir + "\\sr_import_audit_test_bg.json", out = dir + "\\sr_import_audit_test.json";
  // Before the first call: the side thread's own file and a period that never fires during the test.
  _putenv_s("SR_AUDIT_IMPORTS_OUT", bg.c_str());
  _putenv_s("SR_AUDIT_IMPORTS_PERIOD", "3600");
  A::importAuditResetForTest();

  for (uint32_t i = 0; i < 20; ++i) A::importCall("SrAuditA", 0x82000000u + 4 * i, 0x100 + i, 1, 2, 3, 4, 5, i % 10, true);
  for (int i = 0; i < 3; ++i) A::importCall("SrAuditB", 0x82100000u, 0, 0, 0, 0, 0, 0, 0, false);
  K::ExportRegistry::instance().registerExport("SrAuditTestExport", plusOne);
  const uint32_t r = srTestAuditCallKernel("SrAuditTestExport", 0x82123456u, 41);
  const uint32_t r0 = srTestAuditCallKernel("SrAuditNoSuchExport", 0x82123456u, 7);
  (void)A::importEnter("SrAuditNeverReturns", 0x82200000u, 1, 2, 3, 4, 5, 6);
  K::ExportRegistry::instance().registerExport("SrAuditFaulting", faulting);
  const uint32_t rf = srTestAuditCallKernel("SrAuditFaulting", 0x82300000u, 9);

  std::string json = snapshot(out);
  std::printf("[IMPORT_AUDIT_TEST] census %s (%zu bytes)\n", out.c_str(), json.size());
  check(has(json, "\"schema\": \"sr-import-audit/2\"") && has(json, "\"final\": false"),
        "the census JSON is written (schema sr-import-audit/2, not final)");

  const std::string a = recordOf(json, "SrAuditA");
  std::printf("       A: %.300s\n", a.c_str());
  check(has(a, "\"entries\": 20, \"returns\": 20, \"unresolved\": 0"), "A1 20 entries, 20 returns, 0 unresolved");
  check(count(a.substr(0, a.find("\"lr_other\"")), "\", 1]") == 16 && has(a, "\"lr_other\": 4"),
        "A1 16 lr bins of 1 call + lr_other 4");
  check(has(a, "[\"00000000\", 2]") && has(a, "[\"00000007\", 2]") && !has(a, "[\"00000008\"") &&
            has(a, "\"results_other\": 4"),
        "A1 results 0..7 in bins (2 each), 8 and 9 in results_other (4)");
  check(count(a, "{\"lr\": ") == 8 && has(a, "{\"lr\": \"82000000\", \"args\": [\"00000100\", \"00000001\"") &&
            has(a, "\"result\": \"00000007\"") && !has(a, "null"),
        "A1 the first 8 calls kept, with lr, arguments and result");
  check(has(recordOf(json, "SrAuditB"), "\"entries\": 3, \"returns\": 3, \"unresolved\": 3"),
        "A2 calls without an export count as unresolved");
  const std::string c = recordOf(json, "SrAuditTestExport");
  std::printf("       A3: r=%u %.300s\n", r, c.c_str());
  check(r == 42 && has(c, "\"entries\": 1, \"returns\": 1, \"unresolved\": 0") && has(c, "[\"82123456\", 1]") &&
            has(c, "\"args\": [\"00000029\"") && has(c, "\"result\": \"0000002A\""),
        "A3 the real callKernel feeds the census: lr, first argument, result");
  const std::string d = recordOf(json, "SrAuditNoSuchExport");
  check(r0 == 0 && has(d, "\"entries\": 1, \"returns\": 1, \"unresolved\": 1") && has(d, "\"result\": \"00000000\""),
        "A4 the real callKernel with no export: unresolved 1, result 0");
  const std::string e = recordOf(json, "SrAuditNeverReturns");
  check(has(e, "\"entries\": 1, \"returns\": 0") && has(e, "[\"82200000\", 1]") && has(e, "\"result\": null"),
        "A5 an entry without a return: entries 1, returns 0, its lr counted, result null");
  const std::string fr = recordOf(json, "SrAuditFaulting");
  std::printf("       A6: r=%u %.200s\n", rf, fr.c_str());
  check(rf == 0 && has(fr, "\"entries\": 1, \"returns\": 0") && has(fr, "[\"82300000\", 1]"),
        "A6 the real callKernel with a faulting export: counted at entry, no return");

  // A7: a blocked import, read while it blocks and after it returns.
  g_release = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  K::ExportRegistry::instance().registerExport("SrAuditBlocking", blocking);
  uint32_t rb = 0;
  std::thread t([&] { rb = srTestAuditCallKernel("SrAuditBlocking", 0x82400000u, 77); });
  std::string during;
  for (int i = 0; i < 200; ++i) {   // until the call has entered (<= 2 s)
    during = recordOf(snapshot(out), "SrAuditBlocking");
    if (has(during, "\"entries\": 1")) break;
    Sleep(10);
  }
  SetEvent(g_release);
  t.join();
  const std::string after = recordOf(snapshot(out), "SrAuditBlocking");
  std::printf("       A7: during %.120s\n       A7: after  %.120s\n", during.c_str(), after.c_str());
  check(has(during, "\"entries\": 1, \"returns\": 0") && has(after, "\"entries\": 1, \"returns\": 1") && rb == 77,
        "A7 a blocked import counts at entry while it blocks, and returns 1 after");

  // A9: PATHS - the file opens in call order, the guest path read at entry.
  {
    constexpr uint32_t kOa = 0x90700100u, kAnsi = 0x90700110u, kStr = 0x90700120u;
    const bool mem = K::getGuestBase() != nullptr && K::guestVAToHost(kStr) != nullptr;
    if (mem) {
      const char* p = "game:\\DATA\\a\"b.bin";
      const uint32_t n = uint32_t(std::strlen(p));
      K::writeGuestBE32(kOa, 0);
      K::writeGuestBE32(kOa + 4, kAnsi);
      K::writeGuestBE32(kOa + 8, 0x40);
      K::writeGuestBE32(kAnsi, (n << 16) | (n + 1));
      K::writeGuestBE32(kAnsi + 4, kStr);
      std::memcpy(K::guestVAToHost(kStr), p, n);
      A::importCall("NtCreateFile", 0x82500000u, 0x90700200u, 0x80100080u, kOa, 0, 0, 0, 0xC0000034u, true);
      A::importCall("NtOpenFile", 0x82500010u, 0x90700200u, 0x00100001u, kOa, 0, 0, 0, 0, true);
      A::importCall("NtQueryFullAttributesFile", 0x82500020u, kOa, 0x90700300u, 0, 0, 0, 0, 0xC000000Fu, true);
      A::importCall("NtClose", 0x82500030u, kOa, 0, kOa, 0, 0, 0, 0, true);    // not a path export
      A::importCall("NtOpenFile", 0x82500040u, 0x90700200u, 1, 0, 0, 0, 0, 0xC0000033u, true);   // no OA: no record
      K::writeGuestBE32(kOa, 0x00001234u);                                     // a handle-relative name
      A::importCall("NtOpenFile", 0x82500050u, 0x90700200u, 1, kOa, 0, 0, 0, 0, true);
    }
    const std::string j = snapshot(out);
    const size_t at = j.find("\"paths\": [");
    const std::string paths = at == std::string::npos ? std::string() : j.substr(at);
    std::printf("       A9: %.700s\n", paths.c_str());
    check(mem && count(paths, "{\"seq\": ") == 4 && has(j, "\"paths_dropped\": 0"),
          "A9 four path records (NtClose and an NtOpenFile without an OA make none), none dropped");
    check(has(paths, "{\"seq\": 0, \"export\": \"NtCreateFile\", \"lr\": \"82500000\", \"r4\": \"80100080\", "
                     "\"path\": \"game:\\\\DATA\\\\a\\\"b.bin\", \"result\": \"C0000034\"}") &&
              has(paths, "{\"seq\": 1, \"export\": \"NtOpenFile\", \"lr\": \"82500010\", \"r4\": \"00100001\"") &&
              has(paths, "{\"seq\": 2, \"export\": \"NtQueryFullAttributesFile\", \"lr\": \"82500020\", "
                         "\"r4\": \"90700300\", \"path\": \"game:\\\\DATA\\\\a\\\"b.bin\", \"result\": \"C000000F\"}"),
          "A9 each record: the export, lr, r4, the guest path JSON-escaped, the result, in call order");
    check(has(paths, "{\"seq\": 3, \"export\": \"NtOpenFile\", \"lr\": \"82500050\"") &&
              has(paths, "\"path\": \"<root 00001234>game:\\\\DATA"),
          "A9 a handle-relative name is tagged with its root handle");
  }

  // A8: the final flush on the project's exits.
  const std::string cq = dir + "\\sr_import_audit_child_quick.json", ce = dir + "\\sr_import_audit_child_exit.json";
  const bool q = runChild("quick", cq), x = runChild("exit", ce);
  _putenv_s("SR_AUDIT_IMPORTS_OUT", bg.c_str());
  const std::string jq = readAll(cq), je = readAll(ce);
  std::printf("       A8: quick_exit child %s (%zu bytes), exit child %s (%zu bytes)\n", q ? "ran" : "FAILED", jq.size(),
              x ? "ran" : "FAILED", je.size());
  check(q && has(jq, "\"final\": true") && has(recordOf(jq, "SrAuditChild"), "\"entries\": 1, \"returns\": 1"),
        "A8 std::quick_exit: the at_quick_exit flush writes the final census with the call");
  check(x && has(je, "\"final\": true") && has(recordOf(je, "SrAuditChild"), "\"entries\": 1, \"returns\": 1"),
        "A8 exit(): the atexit flush writes the final census with the call");

  std::printf("[IMPORT_AUDIT_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
#endif
}
