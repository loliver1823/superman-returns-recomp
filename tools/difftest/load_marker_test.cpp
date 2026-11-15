// step 4 ruling 12 (2026-10-20): the load-screen markers (kernel/load_marker.hpp) - the PLAYER log profile keeps
// [LOAD_BEGIN] / [LOAD_DONE] / [LOAD_OFF], and the overrides of the game's own functions log only when the game's
// body acts (the generated bodies run on a fake ui holder in flat guest memory, on their no-op / trivial paths).
//
// Run: build/difftest.exe --load-marker-test   (exit code = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/load_marker.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <process.h>

PPC_EXTERN_FUNC(sub_8214F1B0);   // FmuiEnterLoadingScreen (overridden in kernel/load_marker.cpp)
PPC_EXTERN_FUNC(sub_8214E788);   // FmuiHideLoadingScreenWorker (overridden in kernel/load_marker.cpp)

namespace K = sr::kernel;
namespace L = sr::kernel::loadmark;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
  std::fflush(stdout);
}
void put32(uint32_t va, uint32_t v) {
  uint8_t* p = K::guestVAToHost(va);
  if (p) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
}
void put8(uint32_t va, uint8_t v) { if (uint8_t* p = K::guestVAToHost(va)) *p = v; }
uint8_t get8(uint32_t va) { const uint8_t* p = K::guestVAToHost(va); return p ? *p : 0xEE; }
size_t count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  std::istringstream in(text);
  for (std::string ln; std::getline(in, ln);) if (ln.find(needle) != std::string::npos) ++n;
  return n;
}
size_t countBut(const std::string& text, const std::string& needle, const std::string& but) {   // without `but`
  size_t n = 0;
  std::istringstream in(text);
  for (std::string ln; std::getline(in, ln);)
    if (ln.find(needle) != std::string::npos && ln.find(but) == std::string::npos) ++n;
  return n;
}
size_t count2(const std::string& text, const std::string& a, const std::string& b) {   // lines holding both
  size_t n = 0;
  std::istringstream in(text);
  for (std::string ln; std::getline(in, ln);)
    if (ln.find(a) != std::string::npos && ln.find(b) != std::string::npos) ++n;
  return n;
}
// one generated body on a scratch guest stack (as the memcpy-guard test does)
void call(void (*fn)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4) {
  PPCContext c{};
  c.r3.u64 = r3;
  c.r4.u64 = r4;
  c.r1.u64 = 0x8FF00000u;
  c.lr = 0x82AABBCCu;
  fn(c, K::getGuestBase());
}
}  // namespace

int runLoadMarkerTest() {
  std::printf("[LOAD_MARKER_TEST] the load-screen begin / end markers\n");
  g_pass = g_fail = 0;
  namespace fs = std::filesystem;
  const fs::path logPath = fs::temp_directory_path() / ("sr_load_marker_" + std::to_string(_getpid()) + ".log");
  _putenv_s("SR_LOG", "player");
  sr::Log::init(logPath.string().c_str());
  check(sr::Log::playerProfile(), "M0 the log runs the player profile");
  L::resetForTest();

  // ── M1: the writers through the real Log (one load, then a second BEGIN with no DONE: the stuck shape) ────────
  check(L::noteBegin(0, 0x822A280Cu) == 1, "M1a [LOAD_BEGIN] #1");
  check(L::noteDone(0x822A2850u) == 1, "M1b [LOAD_DONE] #1");
  check(L::noteOff(0x82304C18u) == 1, "M1c [LOAD_OFF] #1");
  check(L::noteBegin(2, 0x822A280Cu) == 2, "M1d [LOAD_BEGIN] #2 (no DONE follows)");
  check(std::strcmp(L::modeName(1), "BIZARRO") == 0 && std::strcmp(L::modeName(7), "?") == 0, "M1e mode names");

  // ── M2: the overrides on the game's own bodies (a fake ui at 0x00E00000, its load-screen holder at +0x100) ────
  constexpr uint32_t kUi = 0x00E00000u, kHolder = 0x00E00100u;
  put32(kUi + 0x54u, kHolder);
  put32(kHolder + 0x14u, 0);   // no load-screen object: the hide's dismiss branch is skipped
  // Show while already shown: the game's body does nothing, so no [LOAD_BEGIN]
  put8(kHolder + 0x18u, 1);
  call(sub_8214F1B0, kUi, 0);
  check(get8(kHolder + 0x18u) == 1, "M2a Show while shown: the game's body left the shown byte at 1");
  // Hide while shown: the body clears the byte -> one [LOAD_DONE]
  call(sub_8214E788, kUi, 0);
  check(get8(kHolder + 0x18u) == 0, "M2b Hide while shown: the game's body cleared the shown byte");
  // Hide while not shown: the body does nothing -> no line
  call(sub_8214E788, kUi, 0);
  check(get8(kHolder + 0x18u) == 0, "M2c Hide while hidden: nothing changed");

  // ── M4: the transitions (the overrides' decisions): a call that changes nothing writes nothing ───────────────
  check(!L::onShown(1, 1, 0, 0x82000001u) && !L::onShown(0, 0, 0, 0x82000001u), "M4a Show leaving shown 1 / 0: no line");
  check(L::onShown(0, 1, 1, 0x82000001u), "M4b Show turning shown 0 -> 1: [LOAD_BEGIN] #3");
  check(!L::onHidden(1, 1, 0x82000001u) && !L::onHidden(0, 0, 0x82000001u),
        "M4c Hide leaving the shown byte SET (or clear): no line");
  check(!L::onScreenOff(1, 1, 0x82000002u) && !L::onScreenOff(0, 0, 0x82000002u),
        "M4d vtable+0x24 leaving +0x44 SET (another object, a per-frame call) or clear: no line");
  check(L::onScreenOff(1, 0, 0x82000002u), "M4e +0x44 set -> clear: [LOAD_OFF] #2");

  // ── M5: the backstop cap - lines 1-512, then powers of two (the count still advances) ─────────────────────────
  check(L::logged(1) && L::logged(512) && !L::logged(513) && L::logged(1024) && !L::logged(1025) &&
            L::logged(1u << 31) && !L::logged(0),
        "M5a the cap rule: 1-512, then 1024, 2048, ...");
  L::resetForTest();
  uint32_t last = 0;
  for (int i = 0; i < 600; ++i) last = L::noteOff(0x82000003u);
  check(last == 600, "M5b 600 [LOAD_OFF] events counted");

  // ── M6: a wrapped counter's count 0 writes no line (Astra P2 #4: capped(0) was false) ──────────────────────────
  L::setCountsForTest(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
  check(!L::logged(0), "M6a the cap rule: count 0 is not written");
  check(L::noteBegin(0, 0x82000060u) == 0 && L::noteDone(0x82000061u) == 0 && L::noteOff(0x82000062u) == 0,
        "M6b the three counters wrapped to 0");
  check(L::noteBegin(0, 0x82000063u) == 1, "M6c and count on from 1");

  // ── M7: the BEGIN id and time are one snapshot under contention (Astra P2 #1) ─────────────────────────────────
  // 4 writers x 20000 BEGINs (lines only at 1-512 and the powers of two) while a reader takes snapshots: the ids
  // never go back, one id always carries one time, a later id never an earlier time, and no BEGIN is lost.
  L::resetForTest();
  constexpr int kWriters = 4, kEach = 20000;
  std::atomic<bool> go{false}, stop{false};
  std::atomic<int> bad{0};
  std::atomic<uint64_t> reads{0};
  std::thread reader([&] {
    while (!go.load()) std::this_thread::yield();
    L::BeginSnapshot prev = L::beginSnapshot();
    while (!stop.load()) {
      const L::BeginSnapshot s = L::beginSnapshot();
      if (s.id < prev.id || (s.id == prev.id && s.ns != prev.ns) || (s.id > prev.id && s.ns < prev.ns)) bad++;
      prev = s;
      reads++;
    }
  });
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w)
    writers.emplace_back([&] {
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < kEach; ++i) L::noteBegin(1, 0x82000070u);
    });
  go.store(true);
  for (auto& t : writers) t.join();
  stop.store(true);
  reader.join();
  const L::BeginSnapshot end = L::beginSnapshot();
  check(bad.load() == 0 && reads.load() > 0, "M7a snapshots: ids never go back, one id one time, times in id order");
  check(end.id == uint32_t(kWriters * kEach) && end.ns != 0, "M7b every concurrent BEGIN counted (80000)");
  check(L::noteDone(0x82000071u) == 1, "M7c a DONE after them");

  sr::Log::shutdown();
  _putenv_s("SR_LOG", "");
  std::string text;
  {
    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  check(count(text, "[LOAD_BEGIN] #1 mode=SHIELD lr=822A280C") == 1 &&
            count(text, "[LOAD_BEGIN] #2 mode=ENGAGING lr=822A280C") == 1,
        "M3a the player log keeps both [LOAD_BEGIN] lines (mode, caller)");
  check(count2(text, "[LOAD_DONE] #1 hidden ", "lr=822A2850") == 1 &&
            count(text, " s after [LOAD_BEGIN] #1 lr=822A2850") == 1,
        "M3b [LOAD_DONE] #1 names its [LOAD_BEGIN] and the time since it");
  check(count(text, "[LOAD_OFF] #1 screen off ") >= 1 && count(text, "lr=82304C18") == 1, "M3c [LOAD_OFF] #1 is kept");
  // (a [LOAD_DONE] / [LOAD_OFF] line names its [LOAD_BEGIN] too: count the lines whose TAG is LOAD_BEGIN)
  check(countBut(text, "[BOOT] [LOAD_BEGIN] #", "lr=820000") == 2 &&
            count(text, "[LOAD_BEGIN] #3 mode=BIZARRO lr=82000001") == 1,
        "M3d [LOAD_BEGIN] lines: M1's two and M4's transition, none for a Show while shown");
  check(count(text, "[LOAD_DONE] #2 hidden ") == 1 && count(text, " after [LOAD_BEGIN] #2 lr=82AABBCC") == 1 &&
            countBut(text, "[BOOT] [LOAD_DONE] #", "lr=820000") == 2,
        "M3e the Hide override wrote exactly one [LOAD_DONE] (#2, its caller's lr), none for the Hide while hidden");
  check(count2(text, "[LOAD_OFF] #2 screen off ", "lr=82000002") == 1 && count(text, "lr=82000002") == 1,
        "M4f the one [LOAD_OFF] of M4 is the set -> clear transition");
  check(count(text, "lr=82000003") == 512 && count(text, "[LOAD_OFF] #512 screen off ") == 1 &&
            count(text, "[LOAD_OFF] #513 ") == 0 && count(text, "[LOAD_OFF] #600 ") == 0,
        "M5c the cap: 512 of 600 [LOAD_OFF] lines written (#1-#512), none past #512 before #1024");
  // (M5's [LOAD_OFF] lines NAME "[LOAD_BEGIN] #0" - no BEGIN yet after the reset: count the lines whose TAG is #0)
  check(count(text, "[BOOT] [LOAD_BEGIN] #0 ") == 0 && count(text, "[BOOT] [LOAD_DONE] #0 ") == 0 &&
            count(text, "[BOOT] [LOAD_OFF] #0 ") == 0 && count(text, "lr=82000060") == 0 && count(text, "lr=82000061") == 0 &&
            count(text, "lr=82000062") == 0 && count(text, "[LOAD_BEGIN] #1 mode=SHIELD lr=82000063") == 1,
        "M6d no #0 line after the wrap; the next BEGIN is #1 again");
  check(count(text, "lr=82000070") == 512 + 7 && count(text, "[LOAD_BEGIN] #65536 mode=BIZARRO lr=82000070") == 1 &&
            count(text, "[LOAD_BEGIN] #513 ") == 0,
        "M7d the contention BEGINs: #1-#512 and 1024 ... 65536 written (519 lines)");
  check(count(text, "[LOAD_DONE] #1 hidden ") == 2 && count(text, " after [LOAD_BEGIN] #80000 lr=82000071") == 1,
        "M7e the DONE names the last BEGIN (#80000) of the one snapshot");
  std::error_code ec;
  fs::remove(logPath, ec);   // this test's own temp file

  std::printf("[LOAD_MARKER_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
