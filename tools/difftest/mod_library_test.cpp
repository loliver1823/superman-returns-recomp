// iter-465 modlib: the mod library (kernel/mod_library.{hpp,cpp}; docs/MODS.md "The mod library").
//
// Run: build/difftest.exe --mod-library-test     (exit = failed checks)
//
// What it proves:
//   L/G/S/B  the pure helpers (kernel/mod_library.cpp selfTest): the ten categories, category +
//            search filtering, grid navigation at every edge, size labels, and the input
//            release latch (the game reads a neutral pad while the library is open and until
//            the button that closed it is released);
//   M1..M9   the snapshot over the REAL mod manager on a temp content root: mod.json's
//            browser metadata (category, preview, screenshots, nexus), costume packs inferred
//            as Characters with their suits, conflicts naming the winner, refused manifests
//            (a non-Nexus link, a missing screenshot) shown as problems, and the actions
//            (enable / disable, load order, per-suit inclusion) saved to mods/mods.json with
//            the restart banner raised;
//   R1..R3   the SR_Menu route: "open mods FE" opens the overlay and closes the Apt menu
//            (SR_DTEnd action 0); SR_MOD_LIBRARY=0 keeps the retired Apt list page.
#include "kernel/mod_bridge.hpp"
#include "kernel/mod_library.hpp"
#include "kernel/mod_manager.hpp"
#include "kernel/mod_menu.hpp"
#include "kernel/mod_overlay.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace K = sr::kernel;
namespace ML = sr::kernel::modlib;

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}
void writeFile(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << s;
}
std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
const ML::LibEntry* find(const ML::LibSnapshot& s, const std::string& id) {
  for (const auto& e : s.mods)
    if (e.id == id) return &e;
  return nullptr;
}
}  // namespace

int runModLibraryTest() {
  std::printf("[mod-library-test] pure helpers\n");
  g_fail += ML::selfTest();

  std::printf("[mod-library-test] the snapshot over the mod manager\n");
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / ("sr_modlib_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(root, ec);
  const fs::path mods = root / "mods";
  fs::create_directories(root / "Superman Returns (USA)" / "DATA", ec);
  // alpha: a mission with a preview, two screenshots and a Nexus page; beta shares its asset stem
  writeFile(mods / "alpha" / "mod.json",
            "{\"format\":1,\"name\":\"Alpha Mission\",\"author\":\"Tester\",\"version\":\"1.2.0\","
            "\"category\":\"missions\",\"description\":\"A new mission.\",\"preview\":\"preview.png\","
            "\"screenshots\":[\"shots/one.png\",\"shots/two.jpg\"],"
            "\"nexus\":\"https://www.nexusmods.com/games/mods/1\"}");
  writeFile(mods / "alpha" / "preview.png", "png");
  writeFile(mods / "alpha" / "shots" / "one.png", "png");
  writeFile(mods / "alpha" / "shots" / "two.jpg", "jpg");
  writeFile(mods / "alpha" / "DATA" / "_workinprogress" / "alpha" / "X2_SHARED.rx2", "A");
  writeFile(mods / "beta" / "mod.json", "{\"name\":\"Beta Look\",\"category\":\"visuals\"}");
  writeFile(mods / "beta" / "DATA" / "_workinprogress" / "beta" / "X2_SHARED.rx2", "B");
  // suits: a character pack (category inferred) with two suits
  const std::string pre = K::modAssetPrefix("suits");
  writeFile(mods / "suits" / "mod.json",
            "{\"name\":\"Suit Pack\",\"costumes\":[{\"id\":\"red\",\"name\":\"Red Suit\",\"entity\":\"" + pre +
                "RED_ENT\"},{\"id\":\"blue\",\"name\":\"Blue Suit\",\"entity\":\"" + pre + "BLUE_ENT\"}]}");
  writeFile(mods / "suits" / "DATA" / "_workinprogress" / "suits" / (pre + "RED_ENT.xml"), std::string("<entity/>") + '\0');
  writeFile(mods / "suits" / "DATA" / "_workinprogress" / "suits" / (pre + "BLUE_ENT.xml"), std::string("<entity/>") + '\0');
  // gamma ships its own preview.png + shots/one.png: the same paths as alpha's, but metadata, not content
  writeFile(mods / "gamma" / "mod.json",
            "{\"name\":\"Gamma\",\"category\":\"audio\",\"preview\":\"preview.png\",\"screenshots\":[\"shots/one.png\"]}");
  writeFile(mods / "gamma" / "preview.png", "png");
  writeFile(mods / "gamma" / "shots" / "one.png", "png");
  writeFile(mods / "gamma" / "DATA" / "_workinprogress" / "gamma" / "GAMMA_ONLY.txt", "G");
  // refused manifests: a link that is not Nexus Mods, a screenshot that is not there
  writeFile(mods / "badlink" / "mod.json", "{\"name\":\"Bad Link\",\"nexus\":\"http://example.com/x\"}");
  writeFile(mods / "badshot" / "mod.json", "{\"name\":\"Bad Shot\",\"screenshots\":[\"missing.png\"]}");

  K::ModBootEnv e;
  e.contentRoot = K::modPathU8(root) + "/";
  e.automated = false;
  e.allowStateWrite = true;
  e.hasPort = true;
  e.srPortFeatures = "0";
  K::modOverlayResetForTestEnv(e);

  ML::LibSnapshot s = ML::snapshot();
  const ML::LibEntry* a = find(s, "alpha");
  const ML::LibEntry* b = find(s, "beta");
  const ML::LibEntry* su = find(s, "suits");
  const ML::LibEntry* bl = find(s, "badlink");
  const ML::LibEntry* bs = find(s, "badshot");
  const ML::LibEntry* ga = find(s, "gamma");
  check(a && b && su && bl && bs && ga && s.installed == 6, "M1 six installed mods in the snapshot");
  check(a && a->category == "missions" && a->name == "Alpha Mission" && a->author == "Tester" &&
            a->version == "1.2.0" && a->preview.find("preview.png") != std::string::npos,
        "M2 mod.json name / author / version / category / preview");
  check(a && a->screenshots.size() == 2 && a->screenshots[1].find("two.jpg") != std::string::npos &&
            a->nexusUrl == "https://www.nexusmods.com/games/mods/1",
        "M3 screenshots (host paths, in order) and the Nexus page");
  check(su && su->category == "characters" && su->suits.size() == 2 && su->suits[0].included &&
            su->suits[1].included,
        "M4 a costume pack is a Characters mod listing its suits, all included by default");
  bool aWins = false, bLoses = false;
  if (a) for (const auto& c : a->conflicts) aWins = aWins || (c.wins && c.otherId == "beta");
  if (b) for (const auto& c : b->conflicts) bLoses = bLoses || (!c.wins && c.otherId == "alpha");
  check(aWins && bLoses, "M5 a shared asset stem is a conflict: the mod higher in the order wins");
  bool metaConflict = false;
  if (ga) for (const auto& c : ga->conflicts) metaConflict = metaConflict || c.otherId == "alpha";
  check(ga && !metaConflict && ga->conflicts.empty(),
        "M5b two mods' previews / screenshots at the same paths are metadata, not a conflict");
  check(bl && bl->failed && bl->reason.find("nexus") != std::string::npos && bs && bs->failed &&
            bs->reason.find("screenshot") != std::string::npos,
        "M6 a non-Nexus link and a missing screenshot are refused with a reason");
  check(!s.restartRequired && a && a->mounted && !a->needsRestart, "M7 a fresh boot needs no restart");

  std::string msg;
  check(ML::setEnabled("beta", false, &msg), "M8a disable saves");
  s = ML::snapshot();
  b = find(s, "beta");
  check(b && !b->enabled && b->needsRestart && s.restartRequired &&
            readFile(mods / "mods.json").find("\"disabled\": [\"beta\"]") != std::string::npos,
        "M8b disabled in mods.json, the card says NEEDS RESTART and the banner shows");
  const int before = find(s, "beta")->order;
  check(ML::move("beta", -1, &msg), "M9a move up saves");
  s = ML::snapshot();
  check(find(s, "beta")->order == before - 1, "M9b the load order changed");
  check(ML::setSuitIncluded("suits/red", false, &msg), "M9c suit checkbox saves");
  s = ML::snapshot();
  su = find(s, "suits");
  check(su && !su->suits[0].included && su->suits[1].included &&
            readFile(mods / "mods.json").find("\"disabled_costumes\": [\"suits/red\"]") != std::string::npos,
        "M9d the unchecked suit is persisted by its stable id in disabled_costumes");

  std::printf("[mod-library-test] the SR_Menu route\n");
  K::modMenuSetCatalog(K::modMenuMakeModcoreCatalog());
  _putenv_s("SR_MOD_LIBRARY", "1");
  std::vector<K::ModReply> r;
  K::modBridgeDispatchEx("SR_Menu", "open mods FE", &r);
  std::string ctx;
  const bool opened = ML::takeOpenRequest(&ctx);
  check(opened && ctx == "FE" && r.size() == 1 && r[0].method == "SR_DTEnd" && r[0].args.size() == 3 &&
            r[0].args[2].i == 0,
        "R1 open mods: the library opens, the Apt menu closes back to its caller");
  check(ML::isOpen() && ML::blockGameInput(false), "R2 the game reads a neutral pad while it is open");
  ML::setShown(false);
  (void)ML::blockGameInput(false);
  _putenv_s("SR_MOD_LIBRARY", "0");
  r.clear();
  K::modBridgeDispatchEx("SR_Menu", "open mods FE", &r);
  const K::HostPageForTest p = K::hostPageDecodeForTest(r);
  check(!ML::takeOpenRequest(nullptr) && p.page.rfind("mods", 0) == 0 && p.action == 1,
        "R3 SR_MOD_LIBRARY=0 keeps the retired Apt list page (A/B)");
  _putenv_s("SR_MOD_LIBRARY", "");

  fs::remove_all(root, ec);
  std::printf("[mod-library-test] %s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail;
}
