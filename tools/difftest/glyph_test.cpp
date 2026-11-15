// iter-464 input lane: device-matched button prompts (gfx/glyph_subst.{hpp,cpp};
// docs/INPUT_GLYPHS.md).
//
// Run (from the repo root, or set SR_GLYPHS_DIR):
//   python tools/glyphs/glyph_map.py --payloads archive/v1_input/payloads
//   SR_GLYPH_PAYLOADS=archive/v1_input/payloads build/difftest.exe --glyph-test
//   build/difftest.exe --glyph-preview <outdir>     (raw 128x128 RGBA per icon x set, for
//                                                    tools/glyphs/preview.py)
//
// What it proves, through the REAL module and the SHIPPED data/glyphs:
//   G1  the manifest parses with 0 problems and lists every prompt icon;
//   G2  every PlayStation 4/5, Switch and keyboard recipe builds a 128x128 image with a full
//       mip chain and visible pixels (the keyboard stick body is the one deliberate blank),
//       every referenced PNG decodes; the Xbox set keeps the game's own art (no image);
//   G3  identify(): each icon's real guest bytes (the ui.AST payload span the renderer hashes,
//       exported by glyph_map.py) are recognised as that icon; one flipped byte is not; a
//       wrong size or format is not a candidate;
//   G4  keyboard prompts follow the prompt source (hostinput::promptInputFor - the controls
//       lane's bindings once it registers): same source = same image and variant; A -> K
//       changes the hint_a image and its variant; B -> Mouse2 draws the right-mouse art (not
//       the blank key); a right stick driven by the mouse is ONE mouse glyph, not a cluster;
//   G5  coverage: every input of the controls lane's default layout (docs/PC_CONTROLS.md 1:
//       W A S D, mouse buttons and wheel, Space, E, F, M, Tab, Left Shift/Ctrl/Alt, 1-4,
//       Escape, Return, Backspace, arrows, mouse-look) has art of its own.
#include "gfx/glyph_subst.hpp"
#include "gfx/xenos_tiling.hpp"   // iter-465 polish: G3c lays payloads out tiled
#include "kernel/host_input.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace G = sr::gfx::glyphs;
namespace HI = sr::kernel::hostinput;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name.c_str()); }
  else { g_fail++; std::printf("FAIL: %s\n", name.c_str()); }
}
void setEnv(const char* k, const char* v) {
#ifdef _WIN32
  _putenv_s(k, v ? v : "");
#else
  if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
std::string glyphDir() {
  if (const char* e = std::getenv("SR_GLYPHS_DIR"); e && *e) return e;
  std::error_code ec;
  for (const char* c : {"data/glyphs", "../data/glyphs"})
    if (fs::exists(fs::path(c) / "glyph_map.txt", ec)) return fs::absolute(c, ec).string();
  return "data/glyphs";
}
std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
const char* kSetNames[] = {"xbox", "ps4", "ps5", "switch", "keyboard"};
}  // namespace

int runGlyphTest(const char* previewDir) {
  std::printf("[GLYPH_TEST] prompt-icon identities, recipes, shipped art\n");
  const std::string dir = glyphDir();
  setEnv("SR_GLYPHS_DIR", dir.c_str());
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / "sr_glyph_test";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp / "settings", ec);
  setEnv("SR_CONTENT_DIR", tmp.string().c_str());
  setEnv("SR_INSTANCE_ID", nullptr);
  setEnv("SR_GLYPHS", nullptr);
  HI::resetForTest();
  HI::init();  // player defaults (the default prompt source)

  // ── G1 ──
  std::string errors;
  const int problems = G::parseManifestForTest(slurp(fs::path(dir) / "glyph_map.txt"), &errors);
  check(problems == 0, "G1a manifest parses with 0 problems" + (errors.empty() ? "" : " (" + errors + ")"));
  const bool loaded = G::init();
  check(loaded && G::iconCount() >= 19, "G1b init() loads the shipped manifest (" +
                                            std::to_string(G::iconCount()) + " icons)");

  // Preview only: SR_GLYPH_PREVIEW_LAYOUT=pc shows the keyboard prompts with the controls
  // lane's PC default layout (docs/PC_CONTROLS.md section 1, gameplay context) instead of the
  // default source - what players see once that lane registers its prompt source.
  if (const char* lay = std::getenv("SR_GLYPH_PREVIEW_LAYOUT"); previewDir && lay && std::string(lay) == "pc") {
    HI::setKeyPromptSource([](HI::Control c) -> std::string {
      switch (c) {
        case HI::kA: return "E";
        case HI::kB: return "Mouse2";
        case HI::kX: return "Mouse1";
        case HI::kY: return "Space";
        case HI::kLB: return "Left Ctrl";
        case HI::kRB: return "Left Shift";
        case HI::kLT: return "Mouse3";
        case HI::kRT: return "F";
        case HI::kStart: return "Escape";
        case HI::kBack: return "M";
        case HI::kDpadUp: return "1";
        case HI::kDpadRight: return "2";
        case HI::kDpadLeft: return "3";
        case HI::kDpadDown: return "4";
        case HI::kLStickUp: return "W";
        case HI::kLStickDown: return "S";
        case HI::kLStickLeft: return "A";
        case HI::kLStickRight: return "D";
        case HI::kRStickUp: case HI::kRStickDown: case HI::kRStickLeft: case HI::kRStickRight:
          return "MouseMove";
        default: return {};
      }
    });
    std::printf("  --  preview: keyboard prompts with the PC default layout (gameplay)\n");
  }

  // ── G2 ──
  int built = 0, blanks = 0, keeps = 0;
  bool allOk = true;
  for (uint16_t icon = 1; icon <= G::iconCount(); ++icon)
    for (uint8_t set = 0; set < 5; ++set) {
      const G::Image* img = G::replacement(icon, set);
      const std::string what = std::string(G::iconName(icon)) + "/" + kSetNames[set];
      if (set == 0) {
        if (img) { allOk = false; std::printf("    xbox should keep: %s\n", what.c_str()); }
        ++keeps;
        continue;
      }
      if (!img) {
        // PlayStation / Switch keep the stick art (LS/RS labels over a neutral stick body).
        const std::string n = G::iconName(icon);
        if (n == "hint_ls_label" || n == "hint_rs_label" || n == "hint_stick") { ++keeps; continue; }
        allOk = false;
        std::printf("    no image: %s\n", what.c_str());
        continue;
      }
      bool sizeOk = img->size == 128 && img->levels.size() == 8 && img->levels.back().size() == 4;
      size_t vis = 0;
      for (size_t i = 3; i < img->levels[0].size(); i += 4) vis += img->levels[0][i] > 0;
      const bool blank = vis == 0;
      if (blank) ++blanks;
      if (!sizeOk || (blank && what != "hint_stick/keyboard")) {
        allOk = false;
        std::printf("    bad image: %s size=%u levels=%zu visible=%zu\n", what.c_str(), img->size,
                    img->levels.size(), vis);
      }
      ++built;
      if (previewDir) {
        fs::create_directories(previewDir, ec);
        std::ofstream o(fs::path(previewDir) / (std::string(G::iconName(icon)) + "__" + kSetNames[set] + ".rgba"),
                        std::ios::binary);
        o.write(reinterpret_cast<const char*>(img->levels[0].data()), std::streamsize(img->levels[0].size()));
      }
    }
  check(allOk && built > 0, "G2 " + std::to_string(built) + " replacements built (128px, 8 mips, visible), " +
                                std::to_string(blanks) + " deliberate blank, " + std::to_string(keeps) +
                                " keep the game's art (xbox + PS/Switch sticks)");

  // ── G3 ──
  if (const char* pd = std::getenv("SR_GLYPH_PAYLOADS"); pd && *pd) {
    int found = 0, total = 0;
    bool negOk = true;
    for (uint16_t icon = 1; icon <= G::iconCount(); ++icon) {
      const std::string name = G::iconName(icon);
      const std::string bytes = slurp(fs::path(pd) / (name + ".bin"));
      if (bytes.empty()) continue;
      ++total;
      const uint32_t wh = bytes.size() == 1024 ? 32 : bytes.size() == 4096 ? 64 : 0;
      const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
      const uint16_t got = G::identify(20, wh, wh, p, bytes.size());
      // Icons with identical bytes resolve to the first listed (hint_lt/HintShoulder3 class).
      if (got && std::string(G::iconName(got)) == name) ++found;
      else std::printf("    %s identified as %s\n", name.c_str(), got ? G::iconName(got) : "nothing");
      std::string flipped = bytes;
      flipped[bytes.size() / 2] ^= 0x01;
      if (G::identify(20, wh, wh, reinterpret_cast<const uint8_t*>(flipped.data()), flipped.size())) negOk = false;
      if (G::candidate(18, wh, wh) || G::candidate(20, wh + 1, wh)) negOk = false;
    }
    check(total > 0 && found == total, "G3a the real guest bytes of " + std::to_string(found) + "/" +
                                           std::to_string(total) + " icons are identified as themselves");
    check(negOk, "G3b one flipped byte, another format or size: not identified");

    // G3c (iter-465 polish): the LIVE texture is tiled (r5 roll: [TEX_CACHE] fmt=20 32x32
    // pitch=128 tiled=1). Lay every payload out as the GPU texture holds it (Xenia 2D tiling,
    // pitch 128 texels = 32 blocks) and identify it the way the backend does now. Negative
    // control: hashing the tiled bytes directly (the iter-464 path) identifies nothing.
    int tiledFound = 0, tiledTotal = 0, rawHits = 0;
    for (uint16_t icon = 1; icon <= G::iconCount(); ++icon) {
      const std::string name = G::iconName(icon);
      const std::string bytes = slurp(fs::path(pd) / (name + ".bin"));
      if (bytes.empty()) continue;
      const uint32_t wh = bytes.size() == 1024 ? 32 : 64;
      const uint32_t pitch = 128, bw = 4, bb = 16, blocks = wh / bw;
      const uint32_t span = G::tiledSliceBytes(wh, wh, pitch, bw, bb);
      std::vector<uint8_t> tiled(span, 0xCD);
      for (uint32_t by = 0; by < blocks; ++by)
        for (uint32_t bx = 0; bx < blocks; ++bx) {
          const int32_t off = sr::gfx::xenos_tiling::tiled2D(int32_t(bx), int32_t(by), pitch / bw, 4);
          std::memcpy(tiled.data() + off, bytes.data() + (size_t(by) * blocks + bx) * bb, bb);
        }
      ++tiledTotal;
      const uint16_t got = G::identifyTiled(20, wh, wh, tiled.data(), tiled.size(), pitch, bw, bb);
      if (got && G::fnv1a64(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) ==
                     G::fnv1a64(reinterpret_cast<const uint8_t*>(slurp(fs::path(pd) / (std::string(G::iconName(got)) + ".bin")).data()),
                                bytes.size()))
        ++tiledFound;
      if (G::identify(20, wh, wh, tiled.data(), tiled.size())) ++rawHits;
    }
    check(tiledTotal > 0 && tiledFound == tiledTotal && rawHits == 0,
          "G3c tiled live layout (pitch 128): " + std::to_string(tiledFound) + "/" + std::to_string(tiledTotal) +
              " identified after de-tiling; hashing the tiled bytes directly (iter-464) finds " +
              std::to_string(rawHits));
  } else {
    std::printf("  --  G3 skipped (SR_GLYPH_PAYLOADS unset; tools/glyphs/glyph_map.py --payloads)\n");
  }

  // ── G4 ──
  auto iconId = [](const char* n) -> uint16_t {
    for (uint16_t icon = 1; icon <= G::iconCount(); ++icon)
      if (std::string(G::iconName(icon)) == n) return icon;
    return 0;
  };
  const uint8_t kb = uint8_t(HI::GlyphSet::Keyboard);
  const uint16_t hintA = iconId("hint_a"), hintB = iconId("hint_b"), rsLabel = iconId("hint_rs_label");
  auto pixels = [&](uint16_t icon) {
    const G::Image* im = G::replacement(icon, kb);
    return im ? im->levels[0] : std::vector<uint8_t>();
  };
  HI::setKeyPromptSource(nullptr);
  const std::vector<uint8_t> a0 = pixels(hintA);
  const uint64_t va0 = G::variant(hintA, kb);
  const bool stable = !a0.empty() && pixels(hintA) == a0 && G::variant(hintA, kb) == va0 && va0 != 0 &&
                      G::variant(hintA, uint8_t(HI::GlyphSet::PS4)) == 0;
  const std::vector<uint8_t> b0 = pixels(hintB), rs0 = pixels(rsLabel);
  HI::setKeyPromptSource([](HI::Control c) -> std::string {
    switch (c) {
      case HI::kA: return "K";
      case HI::kB: return "Mouse2";
      case HI::kRStickUp: case HI::kRStickDown: case HI::kRStickLeft: case HI::kRStickRight:
        return "MouseMove";
      default: return HI::defaultPromptInputFor(c);
    }
  });
  const std::vector<uint8_t> a1 = pixels(hintA), b1 = pixels(hintB), rs1 = pixels(rsLabel);
  check(stable && !a1.empty() && a1 != a0 && G::variant(hintA, kb) != va0,
        "G4a the keyboard A prompt follows the source: same source = same image + variant, A -> K = new image + variant");
  // The right-mouse art, composed alone, vs what B -> Mouse2 built; and vs an unbound key.
  HI::setKeyPromptSource([](HI::Control c) -> std::string { return c == HI::kB ? "" : "x"; });
  const std::vector<uint8_t> bUnbound = pixels(hintB);
  check(!b1.empty() && b1 != b0 && b1 != bUnbound, "G4b B -> Mouse2 draws the right-mouse art (not X, not the blank key)");
  // A cluster is a plus of four keys with an EMPTY centre cell; one centred mouse glyph fills
  // the centre (the mouse body).
  auto armOpaque = [](const std::vector<uint8_t>& px, int ax, int ay) {
    // 16x16 block centred at (ax, ay) on the 128 canvas
    int n = 0;
    for (int y = ay - 8; y < ay + 8; ++y)
      for (int x = ax - 8; x < ax + 8; ++x) n += px[(size_t(y) * 128 + x) * 4 + 3] > 64;
    return n;
  };
  const int c0 = rs0.empty() ? -1 : armOpaque(rs0, 64, 64), c1 = rs1.empty() ? -1 : armOpaque(rs1, 64, 64);
  check(!rs1.empty() && rs1 != rs0 && c0 == 0 && c1 > 100,
        "G4c mouse-look on the right stick = one mouse glyph, not a cluster (centre cell " +
            std::to_string(c0) + " -> " + std::to_string(c1) + " opaque px)");
  HI::setKeyPromptSource(nullptr);

  // ── G5 ──
  {
    const char* kLayout[] = {"W", "A", "S", "D", "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5",
                             "WheelUp", "WheelDown", "MouseMove", "Space", "E", "F", "M", "Tab",
                             "Left Shift", "Left Ctrl", "Left Alt", "1", "2", "3", "4", "Escape",
                             "Return", "Backspace", "Up", "Down", "Left", "Right", "left shift",
                             "LEFT CTRL", "mouse1"};
    std::string missing;
    for (const char* in : kLayout) {
      std::string png;
      if (!G::keyArtFor(in, &png) || !fs::exists(fs::path(dir) / png, ec)) missing += std::string(" ") + in;
    }
    std::string dflt;
    const bool unboundBlank = !G::keyArtFor("", &dflt) && !dflt.empty() && !G::keyArtFor("NotAKey", nullptr);
    check(missing.empty() && unboundBlank,
          "G5 every input of the controls lane's default layout has its own art (names case-insensitive, "
          "SDL aliases); unbound/unknown -> the blank key" + (missing.empty() ? "" : "; MISSING:" + missing));
  }

  HI::resetForTest();
  fs::remove_all(tmp, ec);
  std::printf("[GLYPH_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
