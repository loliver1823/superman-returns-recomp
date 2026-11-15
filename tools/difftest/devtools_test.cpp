// iter-463 phase 2: the dev-tool gate (kernel/devtools.{hpp,cpp}; docs/DEVTOOLS_DESIGN.md 1, 3.1).
//
// Run: build/difftest.exe --devtools-test     (exit = failed checks)
//
// What it proves:
//   D0  the REAL boot init with no SR_DEVTOOLS on an automated roll (SR_INSTANCE_ID set, so a
//       player's dev.txt cannot leak in) enables NOTHING: mask 0, every predicate false, no
//       title suffix, no RUN_CLASS sidecar - default OFF;
//   D1  the grammar (devToolsCompute): SR_DEVTOOLS replaces dev.txt, 0 / empty = none,
//       @dev.txt reads the file on purpose, ';' and ',' separate, case-insensitive names,
//       canonical table order, duplicates dropped, unknown names (and "all") reported and
//       ignored, SR_INSTANCE_ID ignores dev.txt, dev.txt uses mods/enabled.txt's grammar
//       (BOM, CRLF, '#' comments, blanks);
//   D2  the predicates: devToolAllowed per tool, the hook aggregate = levelskip OR objstate,
//       the dev-tool-assisted vs natural+hud class, the taint, the title suffix and the
//       RUN_CLASS.json sidecar text;
//   D3  the closed set has no "all" and every row's class is observe | devtool.
// The save-taint path through the real guest wrappers is TDV of --xam-content-test.
#include <filesystem>
#include "kernel/devtools.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
  return s;
}
uint32_t bitOf(const char* name) {
  size_t n = 0;
  const K::DevToolInfo* t = K::devToolTable(&n);
  for (size_t i = 0; i < n; i++)
    if (std::strcmp(t[i].name, name) == 0) return 1u << i;
  return 0;
}
}  // namespace

int runDevtoolsTest() {
  std::printf("[DEVTOOLS_TEST] dev-tool gate, grammar, predicates, tags\n");
#ifdef _WIN32
  _putenv_s("SR_DEVTOOLS", "");          // removes the variable
  _putenv_s("SR_INSTANCE_ID", "devtools-test");
#endif
  // D0: the real eager init, nothing enabled
  K::devToolsInitEager();
  check(K::devToolMask() == 0, "D0a default: mask 0");
  check(!K::devToolAllowed("levelskip") && !K::devToolAllowed("objstate") && !K::devToolAllowed("hud"),
        "D0b default: devToolAllowed false for every tool");
  check(!K::devToolHookNeeded_sub_8223EB08() && !K::devToolHookNeeded_sub_8223ECC0() &&
            !K::devToolHookNeeded_sub_8223F230(),
        "D0c default: the tick hooks are pass-throughs");
  check(std::string(K::devToolsTitleSuffix()).empty() && K::devToolsRunClassJson().empty(),
        "D0d default: no title suffix, no RUN_CLASS sidecar");
  check(!K::devToolsTainted() && K::devToolActs() == 0, "D0e default: untainted, 0 acts");
  check(K::devToolsVirtualPadButtons() == 0, "D0f default: the level-skip virtual pad presses nothing");

  // D1: grammar
  {
    auto d = K::devToolsCompute(nullptr, nullptr, nullptr);
    check(d.tools.empty() && d.source == "none", "D1a no env, no dev.txt -> none");
    const std::string txt = "\xEF\xBB\xBF# my tools\r\nObjState\r\n\r\n  levelskip   # skip\r\nobjstate\r\nbogus\r\n";
    d = K::devToolsCompute(nullptr, nullptr, &txt);
    check(join(d.tools) == "objstate,levelskip" && join(d.unknown) == "bogus" && d.source == "dev.txt",
          "D1b dev.txt: BOM/CRLF/comments/blanks/case, table order, dups dropped, unknown reported");
    d = K::devToolsCompute(nullptr, "run0", &txt);
    check(d.tools.empty() && d.source == "instance", "D1c SR_INSTANCE_ID set: dev.txt ignored");
    d = K::devToolsCompute("levelskip;objstate", "run0", &txt);
    check(join(d.tools) == "objstate,levelskip" && d.source == "env", "D1d SR_DEVTOOLS replaces (even on a roll)");
    d = K::devToolsCompute("LevelSkip , freecam", nullptr, &txt);
    check(join(d.tools) == "levelskip,freecam", "D1e ',' separator and case-insensitive names");
    d = K::devToolsCompute("levelskip+cheats", nullptr, &txt);
    check(join(d.tools) == "levelskip,cheats" && d.unknown.empty(),
          "D1e2 '+' separator (sr_drive's comma-separated gate list names two tools as a+b)");
    d = K::devToolsCompute("0", nullptr, &txt);
    check(d.tools.empty(), "D1f SR_DEVTOOLS=0 -> none (dev.txt not read)");
    d = K::devToolsCompute("", nullptr, &txt);
    check(d.tools.empty(), "D1g SR_DEVTOOLS empty -> none");
    d = K::devToolsCompute("@dev.txt", "run0", &txt);
    check(join(d.tools) == "objstate,levelskip" && d.source == "env:@dev.txt", "D1h SR_DEVTOOLS=@dev.txt reads the file");
    d = K::devToolsCompute("all", nullptr, nullptr);
    check(d.tools.empty() && join(d.unknown) == "all", "D1i there is no 'all'");
  }

  // D1p: the player's default plan / output paths (dev.txt only)
  {
    namespace fs = std::filesystem;
    const std::string root = "C:/games/sr";
    const std::string plan = (fs::path(root) / "levelskip.txt").string();
    const std::string out = (fs::path(root) / "devtools_out").string();
    auto p = K::devToolsPlayerPaths("dev.txt", root, "", "", true);
    check(p.cmds == plan && p.out == out, "D1p1 dev.txt + levelskip.txt present -> the plan and devtools_out");
    p = K::devToolsPlayerPaths("dev.txt", root, "", "", false);
    check(p.cmds.empty() && p.out == out, "D1p2 dev.txt, no levelskip.txt -> no plan (nothing to run)");
    p = K::devToolsPlayerPaths("env:@dev.txt", root, "x.txt", "o", true);
    check(p.cmds == "x.txt" && p.out == "o", "D1p3 the env names win over the defaults");
    p = K::devToolsPlayerPaths("env", root, "", "", true);
    check(p.cmds.empty() && p.out.empty(), "D1p4 an SR_DEVTOOLS list never picks up the player's files");
    p = K::devToolsPlayerPaths("instance", root, "", "", true);
    check(p.cmds.empty() && p.out.empty(), "D1p5 an automated roll (SR_INSTANCE_ID) never picks them up");
    p = K::devToolsPlayerPaths("dev.txt", "", "", "", true);
    check(p.cmds.empty() && p.out.empty(), "D1p6 no content root -> nothing");
  }

  // D2: predicates and tags
  K::devToolsResetForTest(bitOf("objstate"));
  check(K::devToolAllowed("objstate") && !K::devToolAllowed("levelskip"), "D2a objstate only: allowed per tool");
  check(K::devToolHookNeeded_sub_8223EB08(), "D2b objstate needs the tick hook");
  check(!K::devToolsAnyDevtoolClass() && !K::devToolsTainted() &&
            std::string(K::devToolsTitleSuffix()) == " [natural+hud: objstate]",
        "D2c observe-only = natural+hud, untainted");
  check(K::devToolsRunClassJson().empty(), "D2d natural+hud: no present-dump sidecar (its frames stay usable)");
  K::devToolsResetForTest(bitOf("levelskip"));
  check(K::devToolAllowed("levelskip") && K::devToolHookNeeded_sub_8223F230(), "D2e levelskip: allowed + hook");
  check(K::devToolsAnyDevtoolClass() && K::devToolsTainted(), "D2f levelskip = dev-tool-assisted, tainted");
  check(std::string(K::devToolsTitleSuffix()) == " [dev-tool-assisted: levelskip]", "D2g title suffix");
  check(K::devToolsRunClassJson().find("\"dev-tool-assisted\"") != std::string::npos, "D2h dev-tool-assisted sidecar");
  // DT-2 (iter-464): freecam and cheats run their commands on the game's tick thread (the drain point)
  K::devToolsResetForTest(bitOf("freecam"));
  check(K::devToolHookNeeded_sub_8223EB08() && K::devToolsAnyDevtoolClass() && K::devToolsTainted(),
        "D2i freecam: devtool class, tainted, and the tick hook (its command runs on the tick thread)");
  K::devToolsResetForTest(bitOf("cheats"));
  check(K::devToolAllowed("cheats") && !K::devToolAllowed("levelskip") && K::devToolHookNeeded_sub_8223EB08() &&
            K::devToolsAnyDevtoolClass() && K::devToolsTainted(),
        "D2i2 cheats: allowed alone, devtool class, tainted, tick hook");
  check(std::string(K::devToolsTitleSuffix()) == " [dev-tool-assisted: cheats]", "D2i3 cheats title suffix");
  check(!K::devToolsFreecamRouting(), "D2i4 the pad is never rerouted before a freecam command ran");
  {
    std::string err;
    check(K::devToolsSubmit("freecam on", &err) == 0 && !err.empty(),
          "D2i5 a command for a tool that is off is refused at submit (freecam with only cheats on)");
    err.clear();
    check(K::devToolsSubmit("pause_timers sideways", &err) == 0 && !err.empty(), "D2i6 a bad command is refused at submit");
    err.clear();
    check(K::devToolsSubmit("warp 1 2", &err) == 0 && !err.empty(), "D2i7 warp needs three coordinates");
    err.clear();
    check(K::devToolsSubmit("warp 1 2 x", &err) == 0 && !err.empty(), "D2i8 warp refuses a non-number");
    err.clear();
    check(K::devToolsSubmit("warp camera", &err) == 0 && !err.empty(), "D2i9 warp camera needs the freecam tool");
    err.clear();
    check(K::devToolsSubmit("warp -2267.4 1177.0 -411.6", &err) != 0 && err.empty(),
          "D2i10 warp X Y Z with cheats on is queued (it runs on the next active tick)");
    err.clear();
    check(K::devToolsSubmit("infinite_stamina maybe", &err) == 0 && !err.empty(), "D2i11 infinite_stamina takes on|off");
    err.clear();
    check(K::devToolsSubmit("infinite_stamina on", &err) != 0 && err.empty(), "D2i12 infinite_stamina on is queued");
  }
  check(!K::devToolAllowed("nosuchtool") && !K::devToolAllowed(nullptr), "D2j unknown / null tool -> false");
  K::devToolsResetForTest(0);
  check(K::devToolMask() == 0 && !K::devToolHookNeeded_sub_8223EB08(), "D2k reset to none");

  // D3: the closed set
  {
    size_t n = 0;
    const K::DevToolInfo* t = K::devToolTable(&n);
    bool ok = n == 8;
    for (size_t i = 0; i < n; i++)
      ok = ok && std::strcmp(t[i].name, "all") != 0 &&
           (std::strcmp(t[i].cls, "observe") == 0 || std::strcmp(t[i].cls, "devtool") == 0);
    check(ok, "D3 closed set: 8 rows (iter-464 devmenu), classes observe|devtool, no 'all'");
  }
  std::printf("[DEVTOOLS_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
