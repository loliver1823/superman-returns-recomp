// Standalone check of tools/xenia-trace/xenia_trace.h (v2 filters) - no Xenia needed.
// Build + run (any C++17 compiler):
//   clang++ -std=c++17 -O1 tools/xenia-trace/test_xenia_trace.cpp -o test_xenia_trace.exe
//   test_xenia_trace.exe <scratch-dir>
// It sets the XENIA_GUEST_TRACE_* environment BEFORE the singleton is first touched,
// emits a fixed sequence and reads the file back.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#define setenv_(k, v) _putenv_s(k, v)
#else
#define setenv_(k, v) setenv(k, v, 1)
#endif
#include "xenia_trace.h"

using xe::trace::GuestTracer;
using xe::trace::XeTraceRecord;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  std::string path = dir + "/t_xenia_trace.bin";
  std::string list = dir + "/t_vas.txt";
  if (std::FILE* f = std::fopen(list.c_str(), "wb")) {
    std::fputs("# comment 0xDEADBEEF\n0x82722DB8, 82119980;0X8246d5c8\n", f);
    std::fclose(f);
  }
  setenv_("XENIA_GUEST_TRACE", "1");
  setenv_("XENIA_GUEST_TRACE_FILE", path.c_str());
  setenv_("XENIA_GUEST_TRACE_VAS", ("@" + list).c_str());
  setenv_("XENIA_GUEST_TRACE_ICALL_FUNCS", "0x82722DB8");
  setenv_("XENIA_GUEST_TRACE_ICALL_BAD", "1");

  auto& t = GuestTracer::Get();
  CHECK(t.enabled());
  CHECK(t.WantProlog(0x82722DB8));
  CHECK(t.WantProlog(0x82119980));
  CHECK(t.WantProlog(0x8246D5C8));      // upper-case 0X and lower-case hex
  CHECK(!t.WantProlog(0xDEADBEEF));     // inside a comment
  CHECK(!t.WantProlog(0x82000000));
  CHECK(t.WantIcall(0x82722DB8));
  CHECK(t.WantIcall(0x82000000));       // ICALL_BAD instruments everything

  t.EmitFuncEntry(0x82722DB8, 0x82721F28, 3, 0x70000000, 7);           // rec 0
  t.EmitIndirectCall(0x82722DB8, 0x82400000, 0x82722E34, 3, 1, 7);     // rec 1 tag 2 (listed, good)
  t.EmitIndirectCall(0x82722DB8, 0x01010000, 0x82722E34, 3, 1, 7);     // rec 2 tag 3 (listed, bad)
  t.EmitIndirectCall(0x82000000, 0x82400000, 0x82000010, 3, 1, 7);     // dropped (unlisted, good)
  t.EmitIndirectCall(0x82000000, 0xFFFFFFFE, 0x82000010, 3, 1, 7);     // rec 3 tag 3 (unlisted, bad)
  t.EmitIndirectCall(0x82000000, 0x82400002, 0x82000010, 3, 1, 7);     // rec 4 tag 3 (misaligned)
  t.EmitIndirectCall(0x82000000, 0x8290F5B8, 0x82000010, 3, 1, 7);     // rec 5 tag 3 (== CODE_HI)
  t.EmitIndirectCall(0x82000000, 0x820F0000, 0x82000010, 3, 1, 7);     // dropped (== CODE_LO, good)

  // Records are fflush'ed per write when a v2 filter is active: read back now.
  std::vector<XeTraceRecord> recs;
  if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
    char hdr[32];
    CHECK(std::fread(hdr, 1, 32, f) == 32);
    CHECK(std::string(hdr, 8) == "XENTRC01");
    XeTraceRecord r;
    while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
    std::fclose(f);
  } else {
    CHECK(false && "trace file missing");
  }
  CHECK(recs.size() == 6);
  if (recs.size() == 6) {
    CHECK(recs[0].tag == 0 && recs[0].guest_pc == 0x82722DB8 && recs[0].lr == 0x82721F28);
    CHECK(recs[1].tag == 2 && recs[1].guest_pc == 0x82400000 && recs[1].lr == 0x82722E34);
    CHECK(recs[2].tag == 3 && recs[2].guest_pc == 0x01010000);
    CHECK(recs[3].tag == 3 && recs[3].guest_pc == 0xFFFFFFFE);
    CHECK(recs[4].tag == 3 && recs[4].guest_pc == 0x82400002);
    CHECK(recs[5].tag == 3 && recs[5].guest_pc == 0x8290F5B8);
  }
  std::printf("%s (%d failed, %zu records)\n", g_fail ? "FAIL" : "PASS", g_fail, recs.size());
  return g_fail ? 1 : 0;
}
