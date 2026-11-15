// iter-466 map lane: the useful pause map (kernel/map_markers.{hpp,cpp}; docs/MAP.md, docs/MODS.md
// "Map markers and maps for mods").
//
// Run: build/difftest.exe --map-test     (from the repo root; exit = failed checks)
//
// What it proves:
//   D1..D3   the built-in definition (data/port_features/pause_map/map.json) parses: one map, the five
//            types, 9 races + 6 rampages + 100 kittens + story + metro, every marker typed;
//   V1..V9   validation refuses bad data with a located reason (unknown key, bad colour, no position,
//            two positions, duplicate id, a value with two sources, a bad format, a bad view);
//   G1..G4   merge: a mod ADDS a type + marker to "metropolis", a repeated marker id replaces the
//            earlier one, a "view" REPLACES the map (noted), an unknown type drops the marker (noted);
//   E1..E12  evaluation over a fake game: race unlocked by its dependency, PAR time while pending,
//            BEST time once completed, the global rampage best labelled as such, the story trigger
//            (available, current, finished = gone), kittens hidden until WARobj02 then found / not
//            found with the legend count, metro events, a Lua status, "becomes";
//   P1..P4   the projection: the map camera's straight-down view, the 1280x720 stage, behind = none,
//            the SR_MapXY packing;
//   L1..L3   the Lua 5.0.2 global reader over fake guest memory (hash chain, float / bool values,
//            a missing name);
//   B1..B4   the SR_Map bridge: open describes types / markers / info / positions, a tick sends only
//            moved markers, filters persist into the next open;
//   R1..R5   routes (parse, refusal, SR_MapRoute / SR_MapRtXY on select), glyph + count on the wire,
//            a remembered trigger keeps its marker while the world despawns it;
//   C1       legend counts stay with the type's own layer;
//   M1..M4   the mod manager: the example pack (tests/fixtures/mods/map_example) mounts with its
//            map; a bad map refuses the mod with "mod.json: map: ..."; map.json + "map" both refused.
#include "kernel/map_markers.hpp"
#include "kernel/mod_bridge.hpp"
#include "kernel/mod_formats.hpp"
#include "kernel/mod_manager.hpp"
#include "kernel/mod_overlay.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace K = sr::kernel;
namespace MM = sr::kernel::mapmk;

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}
std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void writeFile(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << s;
}

bool parseErr(const std::string& json, const std::string& want) {
  MM::Definition d;
  std::string err;
  const bool ok = MM::parseDefinitionText(json, "t", &d, &err);
  if (ok || err.find(want) == std::string::npos) {
    std::printf("    (got ok=%d err='%s', wanted '%s')\n", ok ? 1 : 0, err.c_str(), want.c_str());
    return false;
  }
  return true;
}

std::string wrap(const std::string& marker, const std::string& types = R"([{"id":"a","label":"A"}])") {
  return R"({"format":1,"maps":[{"id":"m","types":)" + types + R"(,"markers":[)" + marker + "]}]}";
}

struct Fake : MM::GameReader {
  std::map<std::string, int64_t> dasV;
  std::map<std::string, double> stats, luas;
  std::vector<MM::TriggerInfo> trig;
  std::vector<std::string> loaded{"BaseWorldFlow"};
  std::optional<int64_t> das(const std::string& name, const std::string& flow) override {
    auto it = dasV.find(flow.empty() ? name : flow + "/" + name);
    if (it == dasV.end()) return std::nullopt;
    return it->second;
  }
  std::optional<double> stat(const std::string& n) override {
    auto it = stats.find(n);
    return it == stats.end() ? std::nullopt : std::optional<double>(it->second);
  }
  std::optional<double> lua(const std::string& n) override {
    auto it = luas.find(n);
    return it == luas.end() ? std::nullopt : std::optional<double>(it->second);
  }
  const std::vector<MM::TriggerInfo>& triggers() override { return trig; }
  std::vector<std::string> worlds() override { return loaded; }
  bool objectiveRunning() override { return false; }
  void setBit(const std::string& prefix, int bit, bool on = true) {
    int64_t& w = dasV[prefix + std::to_string(bit >> 5)];
    if (on) w |= int64_t(1) << (bit & 31);
    else w &= ~(int64_t(1) << (bit & 31));
  }
  MM::TriggerInfo& addTrigger(int64_t idx, const std::string& type, std::array<float, 3> pos, bool avail) {
    MM::TriggerInfo t;
    t.idx = idx;
    t.type = type;
    t.pos = pos;
    t.hasPos = true;
    t.available = avail;
    trig.push_back(t);
    return trig.back();
  }
};

const MM::Marker* findMk(const MM::Evaluation& ev, const std::string& id) {
  for (const auto& m : ev.markers)
    if (m.id == id) return &m;
  return nullptr;
}

// fake guest memory for the Lua reader: a few pages, big-endian
struct FakeMem {
  std::map<uint32_t, std::vector<uint8_t>> pages;
  void w32(uint32_t va, uint32_t v) {
    auto& p = pages[va >> 12];
    if (p.empty()) p.assign(4096, 0);
    const uint32_t o = va & 0xFFF;
    p[o] = uint8_t(v >> 24); p[o + 1] = uint8_t(v >> 16); p[o + 2] = uint8_t(v >> 8); p[o + 3] = uint8_t(v);
  }
  void w8(uint32_t va, uint8_t v) {
    auto& p = pages[va >> 12];
    if (p.empty()) p.assign(4096, 0);
    p[va & 0xFFF] = v;
  }
  void str(uint32_t va, const std::string& s) { for (size_t i = 0; i < s.size(); ++i) w8(va + uint32_t(i), uint8_t(s[i])); }
  const uint8_t* page(uint32_t pg) const {
    auto it = pages.find(pg);
    return it == pages.end() ? nullptr : it->second.data();
  }
};
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

}  // namespace

// difftest --map-validate <file>: parse one map definition (a map.json, or the "map" object of a
// mod.json saved on its own) and print the located error or a one-line summary; exit 0 = valid.
int runMapValidate(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::printf("[map-validate] cannot open %s\n", path);
    return 2;
  }
  std::string text;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
  std::fclose(f);
  MM::Definition d;
  std::string err;
  if (!MM::parseDefinitionText(text, path, &d, &err)) {
    std::printf("[map-validate] INVALID: %s\n", err.c_str());
    return 1;
  }
  size_t mk = 0;
  for (const auto& m : d.maps) mk += m.markers.size();
  std::printf("[map-validate] OK: %zu map(s), %zu marker(s)\n", d.maps.size(), mk);
  return 0;
}

int runMapMarkersTest() {
  std::printf("[map-test] the built-in definition\n");
  {
    const std::string text = readFile("data/port_features/pause_map/map.json");
    MM::Definition d;
    std::string err;
    const bool ok = !text.empty() && MM::parseDefinitionText(text, "pause_map", &d, &err);
    if (!ok) std::printf("    (%s)\n", text.empty() ? "data/port_features/pause_map/map.json missing (run from the repo root)" : err.c_str());
    check(ok && d.maps.size() == 1 && d.maps[0].id == "metropolis" && d.maps[0].types.size() == 5, "D1 map.json parses: one map, five types");
    int races = 0, ramp = 0, kits = 0, story = 0, metro = 0;
    if (ok)
      for (const auto& m : d.maps[0].markers) {
        races += m.type == "race";
        ramp += m.type == "rampage";
        kits += m.type == "kitten";
        story += m.type == "story" && m.each;
        metro += m.type == "metro" && m.each && m.eachTrigger.filter == "METROEVENT";
      }
    check(races == 9 && ramp == 6 && kits == 100 && story == 1 && metro == 1, "D2 9 races, 6 rampages, 100 kittens, story + metro generators");
    std::vector<std::string> notes;
    auto def = std::make_shared<MM::Definition>(d);
    const auto merged = MM::mergeLayers({{"pause_map", def}}, &notes);
    check(ok && merged.size() == 1 && merged[0].markers.size() == d.maps[0].markers.size() && notes.empty(),
          "D3 every built-in marker names a built-in type");
  }

  std::printf("[map-test] validation\n");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3],"colour":1})"), "markers[0] (x): unknown key 'colour'"), "V1 an unknown marker key is refused, located");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3]})", R"([{"id":"a","label":"A","color":"red"}])"), "types[0]: color must be RRGGBB"), "V2 a bad colour");
  check(parseErr(wrap(R"({"id":"x","type":"a"})"), "exactly one of pos, trigger, each_trigger"), "V3 a marker without a position");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3],"trigger":5})"), "exactly one of pos, trigger, each_trigger"), "V4 two positions");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3]},{"id":"x","type":"a","pos":[1,2,3]})"), "duplicate marker id 'x'"), "V5 a duplicate id");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3],"show":{"das":"a","stat":"b"}})"), "one source"), "V6 a value with two sources");
  check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3],"record":{"stat":"s","format":"minutes"}})"), "format must be count, score or time_ms"), "V7 a bad readout format");
  check(parseErr(R"({"format":1,"maps":[{"id":"m","view":{"kind":"art"}}]})", "view: missing 'movie'"), "V8 an art view without a movie");
  check(parseErr(R"({"format":2,"maps":[]})", "format must be 1") && parseErr("{\"format\":1,", "map: line"), "V9 the format version and JSON syntax");

  std::printf("[map-test] merging layers\n");
  {
    MM::Definition base, add, repl, bad;
    std::string e1, e2, e3, e4;
    const bool p1 = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","view":{"kind":"game"},
      "types":[{"id":"race","label":"RACES"}],"markers":[{"id":"r1","type":"race","pos":[0,0,10]}]}]})", "pause_map", &base, &e1);
    const bool p2 = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis",
      "types":[{"id":"ev","label":"EVENTS","color":"40FF80"}],"markers":[{"id":"e1","type":"ev","pos":[5,0,5]},
      {"id":"r1","type":"race","pos":[1,0,10],"name":"MOVED"}]}]})", "modA", &add, &e2);
    const bool p3 = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","view":{"kind":"art","movie":"art/map","rect":[-8000,-7000,5000,3000]},
      "types":[{"id":"race","label":"RACES"}]}]})", "modB", &repl, &e3);
    const bool p4 = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","markers":[{"id":"z","type":"nosuch","pos":[1,1,1]}]}]})",
                                            "modC", &bad, &e4);
    std::vector<std::string> notes;
    auto mk = [](const MM::Definition& d) { return std::make_shared<MM::Definition>(d); };
    auto merged = MM::mergeLayers({{"pause_map", mk(base)}, {"modA", mk(add)}}, &notes);
    const bool g1 = p1 && p2 && merged.size() == 1 && merged[0].types.size() == 2 && merged[0].markers.size() == 2;
    check(g1, "G1 a mod adds a type and a marker to the built-in map");
    check(g1 && merged[0].markers[0].id == "r1" && merged[0].markers[0].name.s == "MOVED" && notes.size() == 1 &&
              notes[0].find("modA replaces pause_map") != std::string::npos,
          "G2 a repeated marker id replaces the earlier one (noted)");
    notes.clear();
    merged = MM::mergeLayers({{"pause_map", mk(base)}, {"modA", mk(add)}, {"modB", mk(repl)}}, &notes);
    check(p3 && merged.size() == 1 && merged[0].view.kind == "art" && merged[0].markers.empty() && merged[0].source == "modB" &&
              !notes.empty() && notes.back().find("map metropolis: modB replaces") != std::string::npos,
          "G3 a map with a view replaces the map (noted)");
    notes.clear();
    merged = MM::mergeLayers({{"pause_map", mk(base)}, {"modC", mk(bad)}}, &notes);
    check(p4 && merged.size() == 1 && merged[0].markers.size() == 1 && notes.size() == 1 &&
              notes[0].find("unknown type 'nosuch', dropped") != std::string::npos,
          "G4 a marker of an unknown type is dropped (noted), never fatal");
  }

  std::printf("[map-test] evaluation over a fake game\n");
  {
    MM::Definition d;
    std::string err;
    const bool ok = MM::parseDefinitionText(readFile("data/port_features/pause_map/map.json"), "pause_map", &d, &err);
    // add a mod marker with a Lua status and a story beat that becomes a challenge
    MM::Definition modDef;
    const bool okMod = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis",
      "types":[{"id":"ev","label":"EVENTS"}],
      "markers":[{"id":"ev.lua","type":"ev","pos":[10,0,10],"name":"LUA EVENT","completed":{"lua":"EvDone"},"after_complete":"keep",
                  "record":{"lua":"EvBest","format":"score","label":"BEST","when":"completed"}},
                 {"id":"ev.intro","type":"ev","pos":[20,0,20],"name":"INTRO","completed":{"das":"evIntroDone"},"becomes":"ev.repeat"},
                 {"id":"ev.repeat","type":"ev","pos":[20,0,20],"name":"REPEAT","show":{"das":"evIntroDone"},"after_complete":"keep"}]}]})",
                                               "modX", &modDef, &err);
    if (!okMod) std::printf("    (%s)\n", err.c_str());
    const auto maps = MM::mergeLayers({{"pause_map", std::make_shared<MM::Definition>(d)}, {"modX", std::make_shared<MM::Definition>(modDef)}}, nullptr);
    Fake g;
    // chapter-2-like: WARobj02 (6) won, races 95 / 96 / 97 unlocked, 96 completed with 61,230 ms
    g.setBit("objectiveStatus", 6);
    g.setBit("objectiveStatus", 96);
    g.stats["BeginnersBoost"] = 61230;
    g.stats["IndustrialHaste"] = 105000;
    g.stats["BestBizarroScore"] = 1400000;
    g.setBit("objectiveStatus", 67);
    for (int k = 0; k < 4; ++k) g.dasV["kittens" + std::to_string(k)] = 0;
    g.setBit("kittens", 3);
    g.setBit("kittens", 40);
    g.addTrigger(95, "TYPE_MINIGAME", {6052, 70, 63}, true);
    g.addTrigger(96, "TYPE_MINIGAME", {-938, 1170, 1215}, true);
    g.addTrigger(97, "TYPE_MINIGAME", {-7700, 1243, 993}, true);
    g.addTrigger(99, "TYPE_MINIGAME", {4802, 55, 820}, false);
    g.addTrigger(67, "TYPE_MINIGAME", {-79, 902, -993}, true);
    g.addTrigger(68, "TYPE_MINIGAME", {-2916, 1187, 1420}, true);
    auto& st = g.addTrigger(9, "TYPE_OBJECTIVE", {100, 50, 200}, true);
    st.nameKey = "MSTobj01_Name";
    st.textKey = "MSTobj01_Text";
    g.addTrigger(10, "TYPE_OBJECTIVE", {300, 50, 200}, false);   // not available: no marker
    g.addTrigger(11, "TYPE_TRANSITION", {0, 0, 0}, true).nameKey = "T11";   // at the origin: no pin
    auto& me = g.addTrigger(256, "TYPE_OBJECTIVE", {500, 10, 500}, true);
    me.metro = true;
    me.nameKey = "ME01Type01_Name";
    g.luas["EvDone"] = 1;
    g.luas["EvBest"] = 4321;
    g.dasV["evIntroDone"] = 1;
    MM::Evaluation ev = MM::evaluate(maps, g);
    check(ok && okMod && ev.available && ev.mapId == "metropolis", "E1 the built-in map is chosen for the loaded world");
    const MM::Marker* r95 = findMk(ev, "race.95");
    const MM::Marker* r96 = findMk(ev, "race.96");
    const MM::Marker* r99 = findMk(ev, "race.99");
    check(r95 && !(r95->state & MM::kCompleted) && r95->title == "@FastFlyer_Route_4" &&
              r95->desc == "NOT COMPLETED YET  -  PAR TIME 1:45.00",
          "E2 an unlocked race: its name key, NOT COMPLETED YET, the PAR time");
    check(r96 && (r96->state & MM::kCompleted) && r96->desc == "COMPLETED  -  BEST TIME 1:01.23", "E3 a completed race keeps its marker with its BEST time");
    check(!r99, "E4 race 99 stays hidden until 95 + 96 + 97 are all won");
    const MM::Marker* b67 = findMk(ev, "rampage.67");
    const MM::Marker* b68 = findMk(ev, "rampage.68");
    check(b67 && (b67->state & MM::kCompleted) && b67->desc == "STATUE 1 OF 5 COMPLETED  -  BEST (ALL STATUES) 1,400,000" &&
              b68 && b68->desc == "STATUE 2 OF 5  -  TARGET SCORE 1,250,000",
          "E5 rampages: the target while pending, the GLOBAL best labelled as such once done");
    const MM::Marker* s9 = findMk(ev, "story.9");
    check(s9 && s9->title == "@MSTobj01_Name" && s9->desc == "@MSTobj01_Text" && s9->hasPos && !findMk(ev, "story.10"),
          "E6 an available story trigger is marked with the game's own name and text; an unavailable one is not");
    const MM::Marker* s11 = findMk(ev, "story.11");
    check(s11 && !s11->hasPos && !findMk(ev, "story.256"), "E7 a trigger at (0,0,0) gets no pin; metro events are not story");
    const MM::Marker* m256 = findMk(ev, "metro.256");
    check(m256 && m256->title == "@ME01Type01_Name" && m256->hasPos, "E8 a live metro event is marked");
    const MM::Marker* k3 = findMk(ev, "kitten.3");
    const MM::Marker* k4 = findMk(ev, "kitten.4");
    int kitTotal = 0;
    for (const auto& m : ev.markers) kitTotal += m.type == "kitten";
    size_t kt = 0;
    while (kt < ev.types.size() && ev.types[kt].id != "kitten") ++kt;
    check(k3 && (k3->state & MM::kCompleted) && k3->desc == "RESCUED" && k4 && k4->desc == "NOT FOUND YET" && kitTotal == 100 &&
              kt < ev.typeLabels.size() && ev.typeLabels[kt] == "KITTENS 2/100",
          "E9 kittens: found / not found, the legend count 2/100");
    const MM::Marker* lu = findMk(ev, "ev.lua");
    check(lu && (lu->state & MM::kCompleted) && lu->desc == "COMPLETED  -  BEST 4,321", "E10 a mod marker's status and record from Lua globals");
    check(!findMk(ev, "ev.intro") && findMk(ev, "ev.repeat"), "E11 a completed beat that 'becomes' a challenge hands over to it");
    // story completed (no longer available) -> gone; WARobj02 not won -> no kittens
    Fake g2;
    auto& cur9 = g2.addTrigger(9, "TYPE_OBJECTIVE", {100, 50, 200}, false);   // the current objective
    cur9.engaged = true;
    cur9.textKey = "T9";
    MM::Evaluation ev2 = MM::evaluate(maps, g2);
    int kit2 = 0;
    for (const auto& m : ev2.markers) kit2 += m.type == "kitten";
    const MM::Marker* cur = findMk(ev2, "story.9");
    check(kit2 == 0 && cur && (cur->state & MM::kCurrent) && cur->desc == "@T9" && cur->desc2 == "CURRENT OBJECTIVE",
          "E12 no kittens before WARobj02; the engaged objective is CURRENT");
  }

  std::printf("[map-test] the game's projection\n");
  {
    // the map camera: yaw 0, pitch e0 = pi/2 (looking straight down: screen up = world +Z,
    // screen right = world -X), 1280x720, fov 1.7, aspect 16:9
    const float pi = 3.14159265f;
    MM::Camera c = MM::cameraFromEuler(1.7f, false, 16.0f / 9.0f, {0, 2500, 0}, {pi / 2, 0, 0}, 1280, 720);
    float x = 0, y = 0;
    const bool a = MM::project(c, {0, 0, 0}, &x, &y);
    check(a && std::fabs(x - 640) < 0.01f && std::fabs(y - 360) < 0.01f, "P1 the point under the camera is the stage centre");
    float x2 = 0, y2 = 0, x3 = 0, y3 = 0;
    MM::project(c, {0, 0, 500}, &x2, &y2);
    MM::project(c, {500, 0, 0}, &x3, &y3);
    check(std::fabs(x2 - 640) < 0.01f && y2 < 360 && std::fabs(y3 - 360) < 0.01f && x3 < 640,
          "P2 north (+Z) is up, +X is left (the game's north-fixed map)");
    check(!MM::project(c, {0, 3000, 0}, &x, &y), "P3 a point behind the camera has no position");
    const int32_t pk = MM::packXY(123.4f, -56.6f);
    check(pk >= 0 && ((pk >> 15) - 16384) == 123 && ((pk & 0x7FFF) - 16384) == -57 && MM::packXY(NAN, 0) == -1,
          "P4 SR_MapXY packing round-trips; no position = -1");
  }

  std::printf("[map-test] the Lua global reader\n");
  {
    FakeMem m;
    const uint32_t mgr = 0x00472860, L = 0x00AB35C4, gt = 0x00AB38F4, node = 0x00B00000;
    const uint32_t lsize = 3;   // 8 nodes
    m.w32(0x8296D8CC, mgr);
    m.w32(mgr + 0x0C, L);
    m.w32(L + 0x40, 5);
    m.w32(L + 0x44, gt);
    m.w8(gt + 4, 5);
    m.w8(gt + 7, uint8_t(lsize));
    m.w32(gt + 0x10, node);
    auto put = [&](const std::string& name, uint32_t tt, uint32_t val, uint32_t tsAt) {
      const uint32_t h = MM::luaStringHash(name);
      uint32_t n = node + 20 * (h & 7);
      while (m.page(n >> 12) && m.pages[n >> 12][n & 0xFFF] | m.pages[n >> 12][(n & 0xFFF) + 3]) {   // occupied: chain to a free slot
        const uint32_t nx = node + 20 * 7;
        m.w32(n + 16, nx);
        n = nx;
      }
      m.w32(n, 4);
      m.w32(n + 4, tsAt);
      m.w32(n + 8, tt);
      m.w32(n + 12, val);
      m.w32(tsAt + 8, h);
      m.w32(tsAt + 12, uint32_t(name.size()));
      m.str(tsAt + 16, name);
    };
    put("ExampleRooftopDone", 3, fbits(1.0f), 0x00B01000);
    put("ExampleFlag", 1, 1, 0x00B01100);
    auto page = [&m](uint32_t pg) { return m.page(pg); };
    const auto v1 = MM::luaGlobalOver(page, "ExampleRooftopDone");
    const auto v2 = MM::luaGlobalOver(page, "ExampleFlag");
    check(v1 && *v1 == 1.0, "L1 a number global (float 1.0) through the hash chain");
    check(v2 && *v2 == 1.0, "L2 a boolean global");
    check(!MM::luaGlobalOver(page, "NoSuchGlobal"), "L3 a missing global has no value");
  }

  std::printf("[map-test] the SR_Map bridge\n");
  {
    MM::Definition d;
    std::string err;
    MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","types":[{"id":"race","label":"RACES","color":"38B8FF"},
      {"id":"ev","label":"EVENTS","icon":"dot"}],"markers":[{"id":"r","type":"race","pos":[0,0,0],"name":{"text_key":"K"},
      "completed":{"das":"rDone"},"after_complete":"keep"},{"id":"e","type":"ev","pos":[100,0,0],"name":"E"},
      {"id":"far","type":"ev","pos":[0,9000,0],"name":"BEHIND"}]}]})", "t", &d, &err);
    const auto maps = MM::mergeLayers({{"t", std::make_shared<MM::Definition>(d)}}, nullptr);
    Fake g;
    g.dasV["rDone"] = 1;
    MM::Camera cam = MM::cameraFromEuler(1.7f, false, 16.0f / 9.0f, {0, 2500, 0}, {3.14159265f / 2, 0, 0}, 1280, 720);
    MM::mapSetTestReader(&g, &cam, &maps);
    std::vector<K::ModReply> r;
    K::modBridgeDispatchEx("SR_Map", "open", &r);
    auto count = [&](const char* m) { int n = 0; for (const auto& x : r) n += x.method == m; return n; };
    const K::ModReply* mk0 = nullptr;
    const K::ModReply* xy0 = nullptr;
    for (const auto& x : r) {
      if (x.method == "SR_MapMk" && x.args[0].i == 0) mk0 = &x;
      if (x.method == "SR_MapXY" && x.args[0].i == 0) xy0 = &x;
    }
    check(count("SR_MapBegin") == 1 && count("SR_MapType") == 2 && count("SR_MapMk") == 3 && count("SR_MapInfo") == 3 &&
              count("SR_MapEnd") == 1 && mk0 && mk0->args[1].i == (0 | (MM::kCompleted << 8)) && mk0->args[2].s == "@K",
          "B1 open: begin, 2 types, 3 markers + info, end; state + '@key' title on the wire");
    check(xy0 && xy0->args[1].i == 640 + MM::kWireOff && xy0->args[2].i == 360 + MM::kWireOff && count("SR_MapXY") == 3,
          "B2 open sends every position as (i, x + kWireOff, y + kWireOff); behind the camera = kNoPos");
    bool behind = false;
    for (const auto& x : r)
      if (x.method == "SR_MapXY" && x.args[0].i == 2) behind = x.args[1].i == MM::kNoPos && x.args[2].i == MM::kNoPos;
    r.clear();
    K::modBridgeDispatchEx("SR_Map", "tick 640 360", &r);
    const bool still = r.empty();
    cam.pos[0] = 50;
    r.clear();
    K::modBridgeDispatchEx("SR_Map", "tick 600 360", &r);
    check(behind && still && count("SR_MapXY") == 2, "B3 a tick sends only moved markers (nothing when the camera is still)");
    r.clear();
    K::modBridgeDispatchEx("SR_Map", "filter ev 0", &r);
    K::modBridgeDispatchEx("SR_Map", "open", &r);
    bool evHidden = false;
    for (const auto& x : r)
      if (x.method == "SR_MapType" && (x.args[0].i & 0xFFFF) == 1) evHidden = (x.args[0].i >> 16) == 1;
    check(evHidden, "B4 a filter persists into the next open");
    MM::mapSetTestReader(nullptr, nullptr, nullptr);
  }

  std::printf("[map-test] routes, glyphs, counts, remembered triggers\n");
  {
    MM::Definition d;
    std::string err;
    const bool ok = MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","types":[{"id":"race","label":"RACES","glyph":">>"}],
      "markers":[{"id":"r95","type":"race","trigger":95,"completed":{"das":"done95"},"after_complete":"keep",
      "route":[[0,0,0],[100,0,0],[100,0,100]]}]}]})", "pause_map", &d, &err);
    check(ok && d.maps[0].markers[0].route.size() == 3 && d.maps[0].types[0].glyph == ">>", "R1 a marker's route and a type's glyph parse");
    check(parseErr(wrap(R"({"id":"x","type":"a","pos":[1,2,3],"route":[[0,0,0]]})"), "route must be 2..256 points"), "R2 a one-point route is refused");
    const auto maps = MM::mergeLayers({{"pause_map", std::make_shared<MM::Definition>(d)}}, nullptr);
    Fake g;
    g.dasV["done95"] = 0;
    g.addTrigger(95, "TYPE_MINIGAME", {6052, 70, 63}, true);
    MM::Camera cam = MM::cameraFromEuler(1.7f, false, 16.0f / 9.0f, {0, 2500, 0}, {3.14159265f / 2, 0, 0}, 1280, 720);
    MM::mapSetTestReader(&g, &cam, &maps);
    std::vector<K::ModReply> r;
    K::modBridgeDispatchEx("SR_Map", "open", &r);
    auto has = [&](const char* m) -> const K::ModReply* {
      for (const auto& x : r)
        if (x.method == m) return &x;
      return nullptr;
    };
    const K::ModReply* ty = has("SR_MapType");
    const K::ModReply* gl = has("SR_MapGlyph");
    const K::ModReply* ct = has("SR_MapCount");
    check(ty && ty->args[2].s == "RACES" && gl && gl->args[1].s == ">>" && ct && ct->args[1].i == 0 && ct->args[2].i == 1,
          "R3 open sends the label, its glyph and its count (0 of 1) separately");
    r.clear();
    K::modBridgeDispatchEx("SR_Map", "select 0", &r);
    const K::ModReply* rt = has("SR_MapRoute");
    int pts = 0;
    for (const auto& x : r) pts += x.method == "SR_MapRtXY" ? 1 : 0;
    check(rt && rt->args[1].i == 3 && pts == 3, "R4 selecting a marker sends its route (3 points, one SR_MapRtXY each)");
    // the race starts: the world despawns the other triggers; the remembered start keeps the marker
    g.trig.clear();
    r.clear();
    K::modBridgeDispatchEx("SR_Map", "open", &r);
    int mk = 0;
    for (const auto& x : r) mk += x.method == "SR_MapMk" ? 1 : 0;
    check(mk == 1, "R5 a trigger seen earlier keeps its marker while the world despawns it (its own position)");
    MM::mapSetTestReader(nullptr, nullptr, nullptr);
  }

  std::printf("[map-test] legend counts stay with the type's own layer\n");
  {
    MM::Definition base, mod;
    std::string err;
    MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","types":[{"id":"kitten","label":"KITTENS","icon":"dot"}],
      "markers":[{"id":"k1","type":"kitten","pos":[0,0,0],"completed":{"das":"k1"},"after_complete":"keep"},
                 {"id":"k2","type":"kitten","pos":[10,0,0],"completed":{"das":"k2"},"after_complete":"keep"}]}]})", "pause_map", &base, &err);
    MM::parseDefinitionText(R"({"format":1,"maps":[{"id":"metropolis","markers":[{"id":"modk","type":"kitten","pos":[20,0,0],
      "completed":{"const":1},"after_complete":"keep"}]}]})", "a_mod", &mod, &err);
    const auto maps = MM::mergeLayers({{"pause_map", std::make_shared<MM::Definition>(base)}, {"a_mod", std::make_shared<MM::Definition>(mod)}}, nullptr);
    Fake g;
    g.dasV["k1"] = 1;
    g.dasV["k2"] = 0;
    const MM::Evaluation ev = MM::evaluate(maps, g);
    check(ev.markers.size() == 3 && ev.typeDone.size() == 1 && ev.typeDone[0] == 1 && ev.typeTotal[0] == 2,
          "C1 a mod's marker of a built-in type is drawn but not counted (KITTENS 1/2, not 2/3)");
  }

  std::printf("[map-test] the mod manager\n");
  {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("sr_map_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root, ec);
    const fs::path mods = root / "mods";
    fs::create_directories(root / "Superman Returns (USA)" / "DATA", ec);
    const std::string example = readFile("tests/fixtures/mods/map_example/mod.json");
    writeFile(mods / "map_example" / "mod.json", example);
    writeFile(mods / "badmap" / "mod.json", R"({"name":"Bad Map","map":{"format":1,"maps":[{"id":"metropolis","markers":[{"id":"x","type":"t"}]}]}})");
    writeFile(mods / "twomaps" / "mod.json", R"({"name":"Two Maps","map":{"format":1,"maps":[{"id":"m"}]}})");
    writeFile(mods / "twomaps" / "map.json", R"({"format":1,"maps":[{"id":"m"}]})");
    writeFile(mods / "filemap" / "map.json", R"({"format":1,"maps":[{"id":"metropolis","types":[{"id":"f","label":"F"}],
      "markers":[{"id":"f1","type":"f","pos":[1,2,3]}]}]})");
    K::ModBootEnv e;
    e.contentRoot = K::modPathU8(root) + "/";
    e.automated = false;
    e.allowStateWrite = true;
    e.hasPort = true;
    e.srPortFeatures = "0";
    K::modOverlayResetForTestEnv(e);
    const auto list = K::modMenuList();
    auto info = [&](const std::string& id) -> const K::ModInfo* {
      for (const auto& m : list)
        if (m.id == id) return &m;
      return nullptr;
    };
    const K::ModInfo* ex = info("map_example");
    const K::ModInfo* bad = info("badmap");
    const K::ModInfo* two = info("twomaps");
    const K::ModInfo* fm = info("filemap");
    bool layerHasMap = false;
    for (const auto& L : K::modActiveLayers())
      if (L.id == "map_example" && L.map) layerHasMap = true;
    check(!example.empty() && ex && ex->status == K::ModStatus::Ok && ex->map && layerHasMap,
          "M1 the example pack (tests/fixtures/mods/map_example) mounts with its map");
    check(bad && bad->status == K::ModStatus::BadManifest && bad->reason.find("mod.json: map: maps[0] (metropolis).markers[0] (x): a marker needs exactly one of") == 0,
          "M2 a bad map refuses the mod with a located 'mod.json: map: ...' reason");
    check(two && two->status == K::ModStatus::BadManifest && two->reason.find("not both") != std::string::npos,
          "M3 mod.json \"map\" and map.json together are refused");
    check(fm && fm->status == K::ModStatus::Ok && fm->map && fm->map->maps.size() == 1, "M4 a map.json pack (no mod.json) carries its map");
    if (bad) std::printf("    badmap reason: %s\n", bad->reason.c_str());
    fs::remove_all(root, ec);
  }

  std::printf("[map-test] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail;
}
