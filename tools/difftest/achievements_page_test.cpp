// lane E L-a: the ACHIEVEMENTS page's parts without a GPU - the page model (kernel/achievements_page.hpp: rows,
// secrets, the header, the view), the icons (kernel/achievements.hpp imageFromXdbf / iconPng: the XDBF's images), the
// overlay kit's in-memory decode (gfx/overlay_kit.hpp decodeImageBytesForTest: WIC from bytes, scaled, grey) and
// the SR_Achievements fscommand (kernel/mod_bridge.cpp -> the mod library's open request).
//
//   build/difftest.exe --achievements-page-test       exit = failed checks (expect 0)
//   SR_ACH_XDBF=<the title's XDBF, extracted> ...      adds R1-R2 (the real table's icons: every row's image is a
//                                                       PNG that decodes to 64 x 64); skipped (not counted) unset
// The page's look: build/difftest.exe --achievements-render (tools/difftest/mod_library_render.cpp).
#include "kernel/achievements.hpp"
#include "kernel/achievements_page.hpp"
#include "kernel/mod_bridge.hpp"
#include "kernel/mod_library.hpp"
#include "gfx/overlay_kit.hpp"
#include "gfx/achievements_ui.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace A = sr::ach;
namespace AP = sr::kernel::achpage;
namespace ML = sr::kernel::modlib;

int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) {
    g_pass++;
    std::printf("  ok  %s\n", name);
  } else {
    g_fail++;
    std::printf("FAIL: %s\n", name);
  }
}

// A 4 x 2 RGBA PNG: red, green, blue, white / black, grey 128, yellow, transparent cyan.
const uint8_t k4x2[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7F, 0xA8, 0x7D, 0x63, 0x00, 0x00, 0x00, 0x1D, 0x49,
    0x44, 0x41, 0x54, 0x78, 0xDA, 0x1D, 0xC6, 0xB1, 0x01, 0x00, 0x00, 0x0C, 0x82, 0x30, 0x4E, 0xF7, 0x73, 0x6A, 0x25,
    0x0B, 0x08, 0x0E, 0x2E, 0x9A, 0x49, 0x7E, 0x4B, 0x0E, 0x2E, 0x92, 0x12, 0x70, 0x03, 0x9E, 0x72, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
// An 8 x 8 PNG, every pixel (200, 100, 50, 255).
const uint8_t k8x8[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xC4, 0x0F, 0xBE, 0x8B, 0x00, 0x00, 0x00, 0x12, 0x49,
    0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x38, 0x91, 0x62, 0xF4, 0x1F, 0x1F, 0x66, 0x18, 0x19, 0x0A, 0x00, 0xF4, 0x2B,
    0x97, 0x41, 0xA9, 0xFC, 0x51, 0x6D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

std::vector<uint8_t> bytesOf(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

bool px(const std::vector<uint8_t>& v, int w, int x, int y, int r, int g, int b, int a, int tol = 0) {
  const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4u;
  if (i + 3 >= v.size()) return false;
  auto within = [&](int got, int want) { return got >= want - tol && got <= want + tol; };   // ("near" is a windows.h macro)
  return within(v[i], r) && within(v[i + 1], g) && within(v[i + 2], b) && within(v[i + 3], a);
}

// The XDBF layout the core reads (Xenia xdbf_io.h): header, entries {section, id, offset, size}, free list, data.
void put16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  put16(v, x >> 16);
  put16(v, x & 0xFFFF);
}
struct XdbfBuilder {
  struct E {
    uint16_t sec;
    uint64_t id;
    std::vector<uint8_t> data;
  };
  std::vector<E> es;
  void add(uint16_t sec, uint64_t id, std::vector<uint8_t> d) { es.push_back({sec, id, std::move(d)}); }
  std::vector<uint8_t> build() const {
    const uint32_t capacity = uint32_t(es.size()) + 2, freeCap = 1;
    std::vector<uint8_t> v = {'X', 'D', 'B', 'F'};
    put32(v, 0x10000);
    put32(v, capacity);
    put32(v, uint32_t(es.size()));
    put32(v, freeCap);
    put32(v, 0);
    std::vector<uint8_t> data;
    for (const E& e : es) {
      put16(v, e.sec);
      put32(v, uint32_t(e.id >> 32));
      put32(v, uint32_t(e.id));
      put32(v, uint32_t(data.size()));
      put32(v, uint32_t(e.data.size()));
      data.insert(data.end(), e.data.begin(), e.data.end());
    }
    v.resize(v.size() + (capacity - es.size()) * 18 + freeCap * 8, 0);
    v.insert(v.end(), data.begin(), data.end());
    return v;
  }
};

}  // namespace

int runAchievementsPageTest() {
  std::printf("[achievements-page-test] lane E L-a: the ACHIEVEMENTS page (model, icons, decode, open)\n");

  // P: the page model and its view (rows, secrets, header, range, focus / scroll)
  check(AP::selfTest() == 0, "P1 the page model's self test (kernel/achievements_page.cpp selfTest)");

  // D: the kit's in-memory decode, as the image worker does it
  {
    int w = 0, h = 0;
    std::vector<uint8_t> v;
    const bool ok = sr::gfx::overlay::decodeImageBytesForTest(bytesOf(k4x2, sizeof k4x2), 64, 64, &w, &h, &v);
    check(ok && w == 4 && h == 2 && v.size() == 32, "D1 a PNG in memory decodes (4 x 2, RGBA)");
    check(ok && px(v, w, 0, 0, 255, 0, 0, 255) && px(v, w, 1, 0, 0, 255, 0, 255) && px(v, w, 2, 0, 0, 0, 255, 255) &&
              px(v, w, 3, 0, 255, 255, 255, 255) && px(v, w, 0, 1, 0, 0, 0, 255) &&
              px(v, w, 1, 1, 128, 128, 128, 255) && px(v, w, 2, 1, 255, 255, 0, 255),
          "D2 ... its pixels, in order (no channel swap)");
    std::vector<uint8_t> g;
    const bool okG = sr::gfx::overlay::decodeImageBytesForTest(bytesOf(k4x2, sizeof k4x2), 64, 64, &w, &h, &g, true);
    check(okG && px(g, w, 0, 0, 76, 76, 76, 255) && px(g, w, 1, 0, 149, 149, 149, 255) &&
              px(g, w, 2, 0, 28, 28, 28, 255) && px(g, w, 3, 0, 255, 255, 255, 255) &&
              px(g, w, 1, 1, 128, 128, 128, 255) && g[size_t(7) * 4 + 3] == 0,
          "D3 grey: the luminance (77 R + 150 G + 29 B) / 256 in R = G = B, alpha kept");
    std::vector<uint8_t> s;
    const bool okS = sr::gfx::overlay::decodeImageBytesForTest(bytesOf(k8x8, sizeof k8x8), 4, 4, &w, &h, &s);
    check(okS && w == 4 && h == 4 && px(s, w, 2, 2, 200, 100, 50, 255, 2), "D4 larger than the size class: scaled to fit");
    const std::vector<uint8_t> junk = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<uint8_t> head(k4x2, k4x2 + 8);
    check(!sr::gfx::overlay::decodeImageBytesForTest(junk, 64, 64, &w, &h, &s) &&
              !sr::gfx::overlay::decodeImageBytesForTest({}, 64, 64, &w, &h, &s) &&
              !sr::gfx::overlay::decodeImageBytesForTest(head, 64, 64, &w, &h, &s),
          "D5 not an image, nothing, a bare PNG signature: refused");
  }

  // W (the review's P2-4): the image worker itself - imageFromMemory fetches on the worker thread, once per cache
  // entry, and only the grey entry is grey
  {
    static std::atomic<int> fetches{0};
    static std::atomic<bool> onMain{false};
    static const std::thread::id mainId = std::this_thread::get_id();
    auto fetch = [](std::vector<uint8_t>* png) {
      fetches.fetch_add(1);
      if (std::this_thread::get_id() == mainId) onMain.store(true);
      png->assign(k4x2, k4x2 + sizeof k4x2);
      return true;
    };
    namespace OK = sr::gfx::overlay;
    OK::imageFromMemory("pagetest", 64, 64, fetch, false);
    OK::imageFromMemory("pagetest", 64, 64, fetch, true);
    std::vector<OK::DecodedForTest> got = OK::takeDecodedForTest(2, 5000);
    OK::imageFromMemory("pagetest", 64, 64, fetch, false);   // the same entries again: no second fetch
    OK::imageFromMemory("pagetest", 64, 64, fetch, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const OK::DecodedForTest* colour = nullptr;
    const OK::DecodedForTest* grey = nullptr;
    for (const auto& d : got) {
      if (d.key == "mem:pagetest|64") colour = &d;
      if (d.key == "mem:pagetest|grey|64") grey = &d;
    }
    check(got.size() == 2 && colour && grey && colour->ok && grey->ok, "W1 two cache entries (colour, grey), both decoded");
    check(fetches.load() == 2 && !onMain.load(), "W2 ... each fetched once, on the worker thread (never the caller's)");
    check(colour && grey && px(colour->px, colour->w, 0, 0, 255, 0, 0, 255) && px(grey->px, grey->w, 0, 0, 76, 76, 76, 255),
          "W3 ... the colour entry keeps its colour, the grey entry is grey");
  }

  // R (the review's P1-2 / P2-1): the routing - a page closing while another page's request waits keeps it; the 3 s drop
  // latches a held button like a close
  {
    std::string ctx;
    ML::requestOpenPage(ML::OverlayPage::Settings, "IG");
    ML::takeOpenRequest(&ctx);                                  // OPTIONS up
    ML::requestOpenPage(ML::OverlayPage::Achievements, "IG");   // the next page asked while OPTIONS is up / fading
    ML::setShown(false);                                        // OPTIONS' close completes
    const bool kept = ML::isOpen() && ML::openPage() == ML::OverlayPage::Achievements && ML::takeOpenRequest(&ctx);
    ML::setShown(false);
    check(kept && !ML::isOpen(), "R1 a close while another page's request waits: the routing stays shown for that page");
    ML::blockGameInput(false);                                  // clear the close latch
    ML::requestOpenPage(ML::OverlayPage::Achievements, "IG");   // never taken (no frame)
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    const bool b1 = ML::blockGameInput(true);   // the drop, B held
    const bool b2 = ML::blockGameInput(true);   // still held
    const bool b3 = ML::blockGameInput(false);  // released
    check(b1 && b2 && !b3 && !ML::isOpen(), "R2 the 3 s drop with B held: the game sees nothing until B is released");
  }

  // I (the review's P2-2): the page's input edges - held keys / stick at open, repeats
  check(sr::gfx::achui::inputSelfTest() == 0, "I1 keys and the stick held at open do not act until released and re-pressed");

  // X: the icons out of the title database (XDBF section 2, by the XACH row's image id)
  {
    XdbfBuilder b;
    b.add(2, 5, bytesOf(k4x2, sizeof k4x2));
    b.add(2, 6, {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0, 0});   // a JPEG: not what the 360 stores
    b.add(1, 7, bytesOf(k4x2, sizeof k4x2));                          // a PNG, but in section 1 (metadata)
    const std::vector<uint8_t> db = b.build();
    std::vector<uint8_t> out;
    check(A::imageFromXdbf(db, 5, &out) && out == bytesOf(k4x2, sizeof k4x2), "X1 image 5: section 2's PNG, byte for byte");
    check(!A::imageFromXdbf(db, 6, &out), "X2 a section-2 entry that is not a PNG: none");
    check(!A::imageFromXdbf(db, 7, &out) && !A::imageFromXdbf(db, 8, &out), "X3 another section's entry, no entry: none");
    std::vector<uint8_t> cut = db;   // the data is 5, 6, 7 in order: this cuts into image 5's own bytes
    cut.resize(cut.size() - (sizeof k4x2 + 10 + 40));
    const std::vector<uint8_t> notDb = {'N', 'O', 'P', 'E'};
    check(!A::imageFromXdbf(cut, 5, &out) && !A::imageFromXdbf(notDb, 5, &out) && !A::imageFromXdbf(db, 5, nullptr),
          "X4 a truncated database, not an XDBF, no output: none (never a read past the end)");
    A::resetForTest();
    A::setTableForTest(db);
    check(A::iconPng(5, &out) && out == bytesOf(k4x2, sizeof k4x2) && !A::iconPng(6, &out),
          "X5 iconPng reads the title database the core uses (the test seam here)");
    A::resetForTest();
  }

  // O: SR_Achievements (the pause row's command) asks the mod library to open the page, in game by default
  {
    std::vector<sr::kernel::ModReply> rs;
    const sr::kernel::ModBridgeResult r = sr::kernel::modBridgeDispatchEx("SR_Achievements", "", &rs, nullptr);
    std::string ctx;
    const bool open = ML::isOpen() && ML::openPage() == ML::OverlayPage::Achievements && ML::takeOpenRequest(&ctx);
    check(r == sr::kernel::ModBridgeResult::Handled && rs.empty() && open && ctx == "IG",
          "O1 SR_Achievements: the ACHIEVEMENTS page asked for (IG), no reply");
    ML::setShown(false);
    sr::kernel::modBridgeDispatchEx("SR_Achievements", "FE", &rs, nullptr);
    const bool fe = ML::takeOpenRequest(&ctx) && ctx == "FE";
    ML::setShown(false);
    check(fe, "O2 ... 'FE' names the main menu");
    // only the pause row opens it (L-c: L-a's developer aid SR_ACH_PAGE_ON_PAUSE is gone): a pause opens nothing
    sr::kernel::modBridgeDispatchEx("SR_Log", "pause install", &rs, nullptr);
    check(!ML::takeOpenRequest(&ctx) && !ML::isOpen(), "O3 a pause by itself opens nothing (no developer aid)");
  }

  // R: the real table (SR_ACH_XDBF): every row's icon is a PNG in the database that decodes to 64 x 64
  if (const char* real = std::getenv("SR_ACH_XDBF")) {
    std::ifstream f(real, std::ios::binary);
    const std::vector<uint8_t> db((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const A::Table t = A::parseTable(db, 0);
    int found = 0, decoded = 0;
    for (const A::Achievement& a : t.list) {
      std::vector<uint8_t> png, rgba;
      int w = 0, h = 0;
      if (!A::imageFromXdbf(db, a.imageId, &png)) continue;
      ++found;
      if (sr::gfx::overlay::decodeImageBytesForTest(png, 64, 64, &w, &h, &rgba) && w == 64 && h == 64) ++decoded;
    }
    std::printf("       R: %zu rows, %d icons found, %d decoded to 64 x 64\n", t.list.size(), found, decoded);
    check(t.ok && t.list.size() == 18, "R1 the real table: 18 rows");
    check(found == 18 && decoded == 18, "R2 ... each row's image is a PNG in section 2 and decodes to 64 x 64");
  } else {
    std::printf("  skip R1-R2 (SR_ACH_XDBF unset: the icons are checked live, [ACH_PAGE] icons line)\n");
  }

  std::printf("[achievements-page-test] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
