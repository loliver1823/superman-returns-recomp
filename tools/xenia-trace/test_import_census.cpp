// Standalone check of tools/xenia-trace/sr_import_census.h (the Xenia half of the logic audit's import census) -
// no Xenia needed. Build + run (any C++17 compiler):
//   clang++ -std=c++17 -O1 tools/xenia-trace/test_import_census.cpp -o test_import_census.exe
//   test_import_census.exe <scratch-dir>
// It sets XENIA_IMPORT_CENSUS before the singleton is first touched, drives Enter / Leave the way the patched
// trampoline does (a fake PPCContext with r[32] and lr), writes the census and reads it back.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#ifdef _WIN32
#define setenv_(k, v) _putenv_s(k, v)
#else
#define setenv_(k, v) setenv(k, v, 1)
#endif
#include "sr_import_census.h"

namespace {
struct FakeCtx {
  uint64_t r[32] = {};
  uint64_t lr = 0;
};
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  ok ? ++g_pass : ++g_fail;
}
std::string rec(const std::string& j, const char* name) {
  const size_t a = j.find(std::string("{\"name\": \"") + name + "\"");
  if (a == std::string::npos) return {};
  return j.substr(a, j.find('\n', a) - a);
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  const std::string path = dir + "/t_import_census.json";
  setenv_("XENIA_IMPORT_CENSUS", path.c_str());
  setenv_("XENIA_IMPORT_CENSUS_PERIOD", "3600");
  namespace C = xe::sr_census;
  check(C::Census::Get() != nullptr, "the census is on when XENIA_IMPORT_CENSUS names a file");

  FakeCtx ctx;
  // A value-returning export (module 0 = xboxkrnl, ordinal 0x123): 20 callers, results 0..9 twice.
  for (uint32_t i = 0; i < 20; ++i) {
    C::Slot* s = C::SlotFor<0, 0x123>("NtTestExport");
    ctx.lr = 0x82000000u + 4 * i;
    for (int k = 0; k < 6; ++k) ctx.r[3 + k] = 0x100 + i + k;
    const int f = C::Enter(s, &ctx);
    ctx.r[3] = i % 10;   // what result.Store leaves in r3
    C::Leave(s, f, uint32_t(ctx.r[3]), true);
  }
  // A void export (module 1 = xam) and a call that never returns.
  {
    C::Slot* v = C::SlotFor<1, 0x45>("XamVoidExport");
    ctx.lr = 0x82100000u;
    C::Leave(v, C::Enter(v, &ctx), 0, false);
    C::Slot* n = C::SlotFor<0, 0x0D>("ExTerminateThreadLike");
    ctx.lr = 0x82200000u;
    (void)C::Enter(n, &ctx);
  }
  check(C::SlotFor<0, 0x123>("NtTestExport") == C::SlotFor<0, 0x123>("NtTestExport"), "one slot per export");
  check(C::Census::Get()->Write(), "Write() succeeds");
  std::ifstream in(path, std::ios::binary);
  const std::string j((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::printf("  census: %zu bytes\n", j.size());
  const std::string a = rec(j, "NtTestExport");
  std::printf("  %.240s\n", a.c_str());
  check(has(j, "\"schema\": \"sr-import-audit/2\"") && has(j, "\"final\": false"), "schema sr-import-audit/2");
  check(has(a, "\"module\": 0, \"ordinal\": 291, \"void\": false, \"entries\": 20, \"returns\": 20"),
        "the value export: module, ordinal, entries, returns");
  check(has(a, "\"lr_other\": 4") && has(a, "[\"00000000\", 2]") && has(a, "\"results_other\": 4"),
        "lr bins (16 + other 4) and result bins (8 + other 4)");
  check(has(a, "{\"lr\": \"82000000\", \"args\": [\"00000100\", \"00000101\", \"00000102\", \"00000103\", "
               "\"00000104\", \"00000105\"], \"result\": \"00000000\"}"),
        "the first call: lr, r3-r8, result");
  const std::string v = rec(j, "XamVoidExport");
  check(has(v, "\"module\": 1, \"ordinal\": 69, \"void\": true, \"entries\": 1, \"returns\": 1") &&
            has(v, "\"results\": []") && has(v, "\"result\": null"),
        "a void export: no results");
  check(has(rec(j, "ExTerminateThreadLike"), "\"entries\": 1, \"returns\": 0"),
        "a call that never returns: counted at entry");
  std::printf("[IMPORT_CENSUS_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
