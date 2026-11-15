// iter-464 perf lane: self-test of the log profiles (app/log.cpp, SR_LOG=dev|player).
//
//   clang++ -std=c++20 -O2 -I. tests/perf/log_profile_selftest.cpp app/log.cpp -o <exe>
//   <exe>            -> exit code = number of failed checks (0 = pass)
//
// Checks: the line format the tools parse is unchanged ("[YYYY-MM-DD HH:MM:SS.mmm] [SEV  ]
// [CAT] msg"); the dev profile keeps every line; the player profile keeps milestone tags,
// drops Info/Debug before formatting, keeps the first 4 Warn and 64 Error lines of a call
// site; SR_INSTANCE_ID selects dev when SR_LOG is unset; the crash tail ring returns the
// last lines in order.

#include "app/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...)                                   \
  do {                                                  \
    if (!(c)) {                                         \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

static std::vector<std::string> readLines(const char* p) {
  std::vector<std::string> v;
  std::ifstream f(p);
  std::string l;
  while (std::getline(f, l)) v.push_back(l);
  return v;
}

static int countSub(const std::vector<std::string>& v, const char* s) {
  int n = 0;
  for (auto& l : v)
    if (l.find(s) != std::string::npos) ++n;
  return n;
}

// One process runs one profile (the profile latches in Log::init); the parent re-runs this
// exe per mode.
static int runMode(const char* mode, const char* path) {
  sr::Log::init(path);
  // Milestones (kept in both profiles).
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NAT_FE] #%u natural FE master tick", 1u);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[APT_REQ] #%u path='screens/hud/main_hud' holder=0", 7u);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GAUGE] t | feTick=%d", 3);
  // Diagnostics: Info (dropped in player), Warn x10, Error x100 from one call site each.
  for (int i = 0; i < 20; ++i) SR_LOG_BOOT(sr::LogSeverity::Info, "[FETCH0_WRITE] i=%d", i);
  for (int i = 0; i < 10; ++i) SR_LOG_BOOT(sr::LogSeverity::Warn, "[IMLOAD_ID] w=%d", i);
  for (int i = 0; i < 100; ++i) SR_LOG_BOOT(sr::LogSeverity::Error, "[MPFLAG] e=%d", i);
  // An untagged line and a tag that merely starts like a milestone.
  SR_LOG_KERNEL_IMPORT("%s(r3=%08X) -> %08X", "KeQueryPerformanceFrequency", 1u, 2u);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NAT_FEX] not a milestone");
  char tail[16384];
  sr::Log::getTailLines(tail, sizeof(tail));
  sr::Log::shutdown();

  auto v = readLines(path);
  const std::regex fmt(R"(^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] \[(TRACE|DEBUG|INFO |WARN |ERROR)\] \[[A-Z:]+\] .*)");
  for (auto& l : v) CHECK(std::regex_match(l, fmt), "%s: line format: %s", mode, l.c_str());
  CHECK(countSub(v, "[LOG_PROFILE]") == 1, "%s: one LOG_PROFILE line", mode);
  CHECK(countSub(v, "[NAT_FE] #1 natural") == 1, "%s: NAT_FE kept", mode);
  CHECK(countSub(v, "[APT_REQ] #7") == 1, "%s: APT_REQ kept", mode);
  CHECK(countSub(v, "[GAUGE]") == 1, "%s: GAUGE kept", mode);
  const bool player = std::strcmp(mode, "player") == 0 || std::strcmp(mode, "plain") == 0;
  if (player) {
    CHECK(countSub(v, "[LOG_PROFILE] player") == 1, "%s: profile=player", mode);
    CHECK(countSub(v, "[FETCH0_WRITE]") == 0, "%s: Info diagnostics dropped (%d)", mode,
          countSub(v, "[FETCH0_WRITE]"));
    CHECK(countSub(v, "[IMLOAD_ID]") == 4, "%s: first 4 Warn of a site (%d)", mode, countSub(v, "[IMLOAD_ID]"));
    CHECK(countSub(v, "[MPFLAG]") == 64, "%s: first 64 Error of a site (%d)", mode, countSub(v, "[MPFLAG]"));
    CHECK(countSub(v, "KeQueryPerformanceFrequency") == 0, "%s: untagged Info dropped", mode);
    CHECK(countSub(v, "[NAT_FEX]") == 0, "%s: NAT_FEX is not NAT_FE", mode);
  } else {
    CHECK(countSub(v, "[LOG_PROFILE] dev") == 1, "%s: profile=dev", mode);
    CHECK(countSub(v, "[FETCH0_WRITE]") == 20, "%s: dev keeps all Info", mode);
    CHECK(countSub(v, "[IMLOAD_ID]") == 10, "%s: dev keeps all Warn", mode);
    CHECK(countSub(v, "[MPFLAG]") == 100, "%s: dev keeps all Error", mode);
    CHECK(countSub(v, "KeQueryPerformanceFrequency") == 1, "%s: dev keeps untagged", mode);
    // Tail: the last 64 lines, oldest first, ending with the NAT_FEX line.
    std::string t(tail);
    CHECK(t.find("[NAT_FEX] not a milestone\n") != std::string::npos &&
              t.rfind("[NAT_FEX]") > t.rfind("KeQueryPerformanceFrequency"),
          "%s: tail ring order", mode);
    int lines = 0;
    for (char c : t) lines += c == '\n';
    CHECK(lines == sr::Log::kMaxTailLines, "%s: tail holds %d lines (%d)", mode, sr::Log::kMaxTailLines, lines);
  }
  return g_fail;
}

int main(int argc, char** argv) {
  if (argc >= 3) return runMode(argv[1], argv[2]);
  // Parent: one child per mode.
  struct M { const char* mode; const char* env; };
  const M modes[] = {
      {"dev", "SR_LOG=dev"},
      {"player", "SR_LOG=player"},
      {"rolls", "SR_LOG=|SR_INSTANCE_ID=run0"},   // unset SR_LOG + instance id -> dev
      {"plain", "SR_LOG=|SR_INSTANCE_ID="},       // a plain launch -> player
  };
  int fails = 0;
  for (const M& m : modes) {
    std::string cmd = "cmd /c \"";
    std::string envs = m.env;
    size_t pos = 0;
    while (pos <= envs.size()) {
      size_t bar = envs.find('|', pos);
      std::string kv = envs.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
      cmd += "set " + kv + "&& ";
      if (bar == std::string::npos) break;
      pos = bar + 1;
    }
    char path[256];
    std::snprintf(path, sizeof(path), "log_selftest_%s.log", m.mode);
    cmd += "\"" + std::string(argv[0]) + "\" " + m.mode + " " + path + "\"";
    const int rc = std::system(cmd.c_str());
    std::printf("mode %-6s -> %d failure(s)\n", m.mode, rc);
    fails += rc;
    std::remove(path);
  }
  std::printf("log profile selftest: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
  return fails;
}
