// iter-464 modcore: the mod manager (kernel/mod_manager.{hpp,cpp}, docs/MODS.md) - discovery,
// mod.json, mods/mods.json + the enabled.txt migration, zip mods, port features generated from
// the disc with SRPATCH1, asset-stem conflicts, the menu API and its SR_Mod* FSCommands - plus
// the format helpers (kernel/mod_formats.cpp: JSON, SHA-256, SRPATCH1; kernel/mod_zip.cpp).
//
// Everything runs on a synthetic content root in %TEMP% (a small BGFA "ui.AST" as the disc),
// and the overlay is checked through the real NtOpenFile/NtReadFile exports where it matters.
//
// Run: build/difftest.exe --mod-manager-test   (exit = failed checks)
//      build/difftest.exe --mod-manager-dump <content root> [SR_MODS value|-] [SR_PORT_FEATURES|-] [automated 0|1]
//      (a canonical summary tools/srmods/rt_srmods.py compares with the Python reference)
#include "kernel/mod_formats.hpp"
#include "kernel/mod_costume_data.hpp"
#include "kernel/mod_density.hpp"
#include "kernel/file_io.hpp"
#include "kernel/mod_manager.hpp"
#include "kernel/mod_overlay.hpp"
#include "kernel/mod_zip.hpp"
#include "kernel/mod_menu.hpp"
#include "kernel/mod_bridge.hpp"
#include "kernel/host_pages.hpp"
#include "ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"

PPC_EXTERN_FUNC(sub_826D39A0);
PPC_EXTERN_FUNC(sub_821E6F90);
PPC_EXTERN_FUNC(sub_821F6850);

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace K = sr::kernel;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
constexpr uint32_t kCostumeMgr = 0x90810000u, kCostumeFlows = 0x90810100u;
constexpr uint32_t kCostumeFlow = 0x90810200u, kCostumeDas = 0x90810300u;
int g_costumeFinds = 0, g_costumeSets = 0, g_requestedSlot = 0;
uint32_t g_findCostumeFlow = kCostumeFlow, g_setCostumeResult = 0x90810400u;
bool g_costumeArguments = true;
void costumeFind(PPCContext& __restrict ctx, uint8_t*) {
  ++g_costumeFinds;
  g_costumeArguments = g_costumeArguments && ctx.r3.u32 == kCostumeMgr &&
    std::strcmp(reinterpret_cast<const char*>(K::guestVAToHost(ctx.r4.u32)), "SupermenActorWorldFlow") == 0;
  ctx.r3.u32 = g_findCostumeFlow;
}
void costumeSet(PPCContext& __restrict ctx, uint8_t*) {
  ++g_costumeSets;
  g_costumeArguments = g_costumeArguments && ctx.r3.u32 == kCostumeDas &&
    std::strcmp(reinterpret_cast<const char*>(K::guestVAToHost(ctx.r4.u32)), "SetCostumeNum") == 0;
  g_requestedSlot = ctx.r5.s32;
  ctx.r3.u32 = g_setCostumeResult;
}
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

void writeBin(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(s.data(), static_cast<std::streamsize>(s.size()));
}
std::string readBin(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void le(std::string& s, uint64_t v, int n) { for (int i = 0; i < n; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF); }
void put32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF); }

// A TOC-last named BGFA1.05 (shift 4, wId 8, wOff 4, wSize 4, 16-byte names).
std::string bgfa(const std::vector<std::pair<std::string, std::string>>& ents) {
  std::string s(0x40, '\0');
  std::memcpy(&s[0], "BGFA1.05", 8);
  std::vector<uint64_t> offs;
  for (const auto& e : ents) {
    while (s.size() % 16) s += '\0';
    offs.push_back(s.size());
    s += e.second;
  }
  while (s.size() % 4) s += '\0';
  const size_t tocOff = s.size();
  le(s, 0x12345678u, 4);
  for (size_t i = 0; i < ents.size(); ++i) {
    s += '\x10';
    le(s, K::fnv1a64Upper(ents[i].first), 8);
    le(s, offs[i] >> 4, 4);
    le(s, ents[i].second.size(), 4);
    std::string nm = ents[i].first;
    nm.resize(16, '\0');
    s += nm;
  }
  put32(s, 0x0C, static_cast<uint32_t>(ents.size()));
  put32(s, 0x10, static_cast<uint32_t>(tocOff));
  put32(s, 0x18, static_cast<uint32_t>(s.size() - tocOff));
  s[0x20] = 0; s[0x21] = 0; s[0x22] = 8; s[0x23] = 4; s[0x24] = 4; s[0x25] = 0; s[0x26] = 4;
  put32(s, 0x28, 1);
  put32(s, 0x2C, 16);
  return s;
}

// A compiled (05F5E0FF) XML document in the retail layout: header, {len, off, hash} string table,
// string blob, 12-byte elements {attr offset, count, name, subtree end}, then u16 attribute pairs.
struct CxEl { std::string name; std::vector<std::pair<std::string, std::string>> attrs; uint16_t end; };
std::vector<uint8_t> compiledXml(const std::vector<CxEl>& els) {
  std::vector<std::string> strs;
  auto id = [&](const std::string& s) {
    for (size_t i = 0; i < strs.size(); ++i) if (strs[i] == s) return static_cast<uint16_t>(i);
    strs.push_back(s);
    return static_cast<uint16_t>(strs.size() - 1);
  };
  for (const auto& e : els) { id(e.name); for (const auto& [k, v] : e.attrs) { id(k); id(v); } }
  std::string blob;
  std::vector<std::pair<uint32_t, uint32_t>> lo;
  for (const auto& s : strs) { lo.push_back({uint32_t(s.size()), uint32_t(blob.size())}); blob += s; blob += '\0'; }
  std::vector<uint8_t> b(44, 0);
  auto be = [&](std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = n - 1; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i))); };
  auto set = [&](size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(x >> (8 * (3 - i))); };
  for (const auto& [l, o] : lo) { be(b, l, 4); be(b, o, 4); be(b, 0, 4); }
  b.insert(b.end(), blob.begin(), blob.end());
  const size_t elements = b.size();
  size_t attrAt = elements + 12 * els.size();
  for (const auto& e : els) {
    be(b, uint32_t(attrAt), 4); be(b, uint32_t(e.attrs.size()), 2); be(b, id(e.name), 2); be(b, e.end, 2); be(b, 0, 2);
    attrAt += 4 * e.attrs.size();
  }
  for (const auto& e : els) for (const auto& [k, v] : e.attrs) { be(b, id(k), 2); be(b, id(v), 2); }
  set(0, 0x05F5E0FFu); set(4, 0x14); set(8, uint32_t(elements)); set(16, (uint32_t(els.size()) << 16) | 0x100);
  set(20, 44); set(24, (uint32_t(strs.size()) << 16) | uint32_t(strs.size())); set(32, uint32_t(blob.size()));
  set(36, uint32_t(blob.size())); set(40, 1);
  return b;
}
// A small METROPOLISTRAFFIC in the retail shape (counts 60 / 40 / 60, an event set at 0).
std::vector<uint8_t> trafficDoc(bool withPedestrians = true) {
  std::vector<CxEl> els = {
      {"trafficDef", {{"trafficTypes", "4"}, {"radius", "250"}, {"maxHeightToScale", "200"}}, 0},
      {"trafficSet", {{"type", "Car"}, {"count", "60"}, {"bin", "CIVILIAN|VEHICLE|TRAFFIC"}, {"name", "Car"}}, 2},
      {"traffic", {{"asset", "driving_car_taxi01Ent"}, {"frequency", "6"}}, 2},
      {"trafficSet", {{"type", "ParkedCar"}, {"count", "40"}, {"name", "Parked"}}, 3},
      {"trafficSet", {{"type", "Pedestrian"}, {"radiusScale", "0.8"}, {"count", "60"}, {"name", "Ped"}}, 4},
      {"trafficSet", {{"type", "MEFireTruck"}, {"count", "0"}, {"name", "MEFireTruck"}}, 5}};
  if (!withPedestrians) els[4].attrs[0].second = "Train";
  els[0].end = static_cast<uint16_t>(els.size() - 1);
  return compiledXml(els);
}

std::string uv(uint64_t v) {
  std::string o;
  do { uint8_t b = v & 0x7F; v >>= 7; o += static_cast<char>(v ? (b | 0x80) : b); } while (v);
  return o;
}
std::string sv(int64_t v) { return uv(v >= 0 ? (uint64_t(v) << 1) : ((uint64_t(-v) << 1) - 1)); }
std::string shaRaw(const std::string& s) {
  K::Sha256 h;
  h.update(s.data(), s.size());
  uint8_t d[32];
  h.final(d);
  return std::string(reinterpret_cast<char*>(d), 32);
}
// An SRPATCH1 from a list of raw op bytes (built by the caller).
std::string srpatch(const std::string& base, const std::string& out, const std::string& ops) {
  std::string s = "SRPATCH1";
  le(s, 1, 4); le(s, 0, 4); le(s, base.size(), 8); s += shaRaw(base); le(s, out.size(), 8); s += shaRaw(out);
  return s + ops + std::string(1, '\0');
}

std::string hexToBin(const char* h) {
  std::string o;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) o += static_cast<char>(std::strtoul(std::string(h + i, 2).c_str(), nullptr, 16));
  return o;
}

// A zip with STORED members (the deflate path is checked against Python-made zips by
// tools/srmods/rt_srmods.py; inflate itself below on zlib vectors).
std::string zipStored(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string z, cd;
  for (const auto& f : files) {
    const uint32_t crc = K::crc32Update(0, reinterpret_cast<const uint8_t*>(f.second.data()), f.second.size());
    const size_t off = z.size();
    le(z, 0x04034B50u, 4); le(z, 20, 2); le(z, 0, 2); le(z, 0, 2); le(z, 0, 4); le(z, crc, 4);
    le(z, f.second.size(), 4); le(z, f.second.size(), 4); le(z, f.first.size(), 2); le(z, 0, 2);
    z += f.first + f.second;
    le(cd, 0x02014B50u, 4); le(cd, 20, 2); le(cd, 20, 2); le(cd, 0, 2); le(cd, 0, 2); le(cd, 0, 4); le(cd, crc, 4);
    le(cd, f.second.size(), 4); le(cd, f.second.size(), 4); le(cd, f.first.size(), 2); le(cd, 0, 2); le(cd, 0, 2);
    le(cd, 0, 2); le(cd, 0, 2); le(cd, 0, 4); le(cd, off, 4);
    cd += f.first;
  }
  const size_t cdOff = z.size();
  z += cd;
  le(z, 0x06054B50u, 4); le(z, 0, 2); le(z, 0, 2); le(z, files.size(), 2); le(z, files.size(), 2);
  le(z, cd.size(), 4); le(z, cdOff, 4); le(z, 0, 2);
  return z;
}

K::ModBootEnv envFor(const fs::path& root, const char* srMods, const char* port, bool automated) {
  K::ModBootEnv e;
  e.contentRoot = sr::kernel::modPathU8(root) + "/";
  if (srMods) { e.hasSrMods = true; e.srMods = srMods; }
  if (port) { e.hasPort = true; e.srPortFeatures = port; }
  e.automated = automated;
  e.allowStateWrite = !automated;
  return e;
}

const K::ModInfo* findMod(const std::vector<K::ModInfo>& v, const std::string& id) {
  for (const auto& m : v)
    if (K::modLower(m.id) == K::modLower(id)) return &m;
  return nullptr;
}

std::string summary() {
  std::string o;
  for (const auto& m : K::modMenuList())
    o += "mod " + m.id + " " + K::modStatusName(m.status) + " enabled=" + (m.enabled ? "1" : "0") +
         " mounted=" + (m.mounted ? "1" : "0") + " category=" + m.category + "\n";
  for (const auto& f : K::modMenuFeatures())
    o += "port " + f.id + " " + K::modStatusName(f.status) + " applied=" + (f.applied ? "1" : "0") + "\n";
  for (const auto& L : K::modActiveLayers()) {
    o += std::string("layer ") + (L.port ? "port " : "mod ") + L.id + "\n";
    for (const auto& r : L.roots) {
      std::error_code ec;
      std::vector<std::string> files;
      for (auto it = fs::recursive_directory_iterator(fs::u8path(r), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string fn = sr::kernel::modPathU8(it->path().filename());
        if (fn.empty() || fn[0] == '.' || fn[0] == '@') { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file()) continue;
        const std::string rel = sr::kernel::modPathU8(it->path().lexically_relative(fs::u8path(r)));
        if (K::modLower(rel) == "mod.json") continue;
        const std::string b = readBin(it->path());
        files.push_back("  file " + rel + " " + K::sha256Hex(b.data(), b.size()) +
                        (K::modHiddenHostPath(sr::kernel::modPathU8(it->path())) ? " hidden" : ""));
      }
      std::sort(files.begin(), files.end());
      for (const auto& f : files) o += f + "\n";
    }
  }
  for (const auto& c : K::modMenuConflicts()) o += "conflict " + c.what + " : " + c.winnerId + " > " + c.loserId + "\n";
  return o;
}
}  // namespace

int runModManagerDump(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: --mod-manager-dump <content root> [SR_MODS|-] [SR_PORT_FEATURES|-] [automated 0|1]\n"); return 2; }
  const fs::path root = fs::u8path(argv[2]);
  const char* mods = argc > 3 && std::strcmp(argv[3], "-") != 0 ? argv[3] : nullptr;
  const char* port = argc > 4 && std::strcmp(argv[4], "-") != 0 ? argv[4] : nullptr;
  const bool automated = argc > 5 && argv[5][0] == '1';
  K::ModBootEnv e = envFor(root, mods, port, automated);
  e.allowStateWrite = false;
  K::modManagerResetForTest(e);
  const std::string report = summary();
  const size_t written = std::fwrite(report.data(), 1, report.size(), stdout);
  const int flushed = std::fflush(stdout);
  if (written != report.size() || flushed != 0 || std::ferror(stdout)) {
    std::fprintf(stderr, "mod-manager dump output failed: wrote=%zu expected=%zu flush=%d errno=%d\n",
                 written, report.size(), flushed, errno);
    return 2;
  }
  return 0;
}

int runModManagerTest() {
  std::printf("[MOD_MANAGER_TEST] mods/ discovery, mod.json, mods.json, zips, port features, conflicts, menu\n");
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / ("sr_modmgr_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(root, ec);
  const fs::path disc = root / "Superman Returns (USA)";
  const fs::path mods = root / "mods";

#ifdef _WIN32
  // Product support is built in. An opt-out is solely for developer A/B checks.
  {
    const char* oldGate = std::getenv("SR_MOD_MANAGER");
    const bool hadGate = oldGate != nullptr;
    const std::string savedGate = oldGate ? oldGate : "";
    _putenv_s("SR_MOD_MANAGER", "");
    check(K::modManagerOverlayEnabled(), "B1 no launch flag needed for the built-in mod framework");
    _putenv_s("SR_MOD_MANAGER", "0");
    check(!K::modManagerOverlayEnabled(), "B2 framework developer opt-out is retained");
    _putenv_s("SR_MOD_MANAGER", "1");
    check(K::modManagerOverlayEnabled(), "B3 explicit framework enable is still supported");
    _putenv_s("SR_MOD_MANAGER", hadGate ? savedGate.c_str() : "");
  }
  // ---- L: the costume switcher's per-tick state allocates nothing (step4 lane B, 2026-10-20) -------------------
  // The game collects its Lua heap only when it loads a script (ENGINE_MAP 6c): a 'process' state that builds a
  // closure or a table on every tick fills the Gameplay::FSM pool in minutes. The helpers live at chunk level.
  {
    const char* oldLegacy = std::getenv("SR_COSTUME_SWITCH_LEGACY");
    const std::string savedLegacy = oldLegacy ? oldLegacy : "";
    _putenv_s("SR_COSTUME_SWITCH_LEGACY", "");
    std::string hoisted, legacy, err;
    const bool ok = K::modCostumeSwitchText(&hoisted, &err);
    const size_t mainAt = hoisted.find("function main(mode)");
    const std::string body = mainAt == std::string::npos ? std::string() : hoisted.substr(mainAt);
    size_t functions = 0;
    for (size_t at = body.find("function"); at != std::string::npos; at = body.find("function", at + 1)) ++functions;
    check(ok && mainAt != std::string::npos && hoisted.find("main(") == mainAt + 9,
          "L1 the composed switcher's first \"main(\" is its main (AptScriptLoadAndExecute renames the first one)");
    check(ok && functions == 1 && body.find('{') == std::string::npos,
          "L2 main(mode) builds no closure and no table: its only 'function' is its own declaration");
    bool helpersFirst = ok;
    for (const char* h : {"local function srExists(", "local function srHandOver(", "local function srPark(",
                          "local function srSpawnCharacter(", "local function srRestoreSuperman(",
                          "local function srSaveHasSuit("})
      helpersFirst = helpersFirst && hoisted.find(h) != std::string::npos && hoisted.find(h) < mainAt;
    check(helpersFirst, "L3 the six helpers are chunk-level locals, before main");
    _putenv_s("SR_COSTUME_SWITCH_LEGACY", "1");
    const bool okLegacy = K::modCostumeSwitchText(&legacy, &err);
    const size_t preludeLen = hoisted.find("\tlocal function srExists(") == std::string::npos ? 0
        : hoisted.rfind('\n', hoisted.find("\t-- Character switch (docs/MODS.md")) + 1;
    auto lines = [](const std::string& s) {
      std::vector<std::string> v;
      size_t p = 0;
      while (p < s.size()) { const size_t q = s.find('\n', p); v.push_back(s.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; }
      std::sort(v.begin(), v.end());
      return v;
    };
    check(okLegacy && legacy.rfind("function main(mode)", 0) == 0 && legacy.find("local function srExists(") > 0 &&
              legacy.size() + preludeLen == hoisted.size() && lines(legacy) == lines(hoisted.substr(preludeLen)),
          "L4 SR_COSTUME_SWITCH_LEGACY=1 composes the old text (helpers inside main): the same lines, reordered");
    _putenv_s("SR_COSTUME_SWITCH_LEGACY", savedLegacy.c_str());
  }
#endif

  // ---- F: formats -------------------------------------------------------------
  {
    K::JsonValue j;
    std::string err;
    check(K::jsonParse("\xEF\xBB\xBF{\"a\": [1, -2.5e1, true, null], \"s\": \"x\\u00e9\\ud83d\\ude00\\n\"}", &j, &err) &&
              j.get("a")->arr.size() == 4 && j.get("a")->arr[0].isInteger() && j.get("a")->arr[1].num == -25.0 &&
              j.get("s")->str == "x\xC3\xA9\xF0\x9F\x98\x80\n",
          "F1 JSON: BOM, numbers, literals, \\u escapes incl. a surrogate pair -> UTF-8");
    check(!K::jsonParse("{\"a\": 1,}", &j, &err) && err.find("line 1") != std::string::npos,
          "F2 JSON: a trailing comma is a syntax error with a position");
    check(K::sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "F3 SHA-256('abc') = the FIPS 180-2 test vector");
    check(K::fnv1a64Upper("ScrPressStart") == K::fnv1a64Upper("SCRPRESSSTART"), "F4 FNV-1a-64 ids are case-insensitive");
    std::vector<uint8_t> out;
    const std::string fixed = hexToBin("cb48cdc9c957c8c020ab320b14caf38b7252b832a8a00200");
    std::string want;
    for (int i = 0; i < 3; ++i) want += "hello hello hello hello zip world\n";
    check(K::inflateBuffer(reinterpret_cast<const uint8_t*>(fixed.data()), fixed.size(), &out, &err) &&
              std::string(out.begin(), out.end()) == want,
          "F5 inflate: a zlib level-9 raw DEFLATE stream (fixed Huffman, back-references)");
    const std::string stored = hexToBin("010d00f2ff73746f72656420626c6f636b21");
    check(K::inflateBuffer(reinterpret_cast<const uint8_t*>(stored.data()), stored.size(), &out, &err) &&
              std::string(out.begin(), out.end()) == "stored block!",
          "F6 inflate: a stored block");
    // SRPATCH1: COPY + ADD + FILL + COPYF
    const std::string base = "0123456789ABCDEF";
    const std::string file = "FILEBYTES-the-container";
    const std::string outS = std::string("0123") + "new!" + std::string(9, 'Z') + "BYTES";
    std::string ops;
    ops += '\x01'; ops += sv(0); ops += uv(4);                      // COPY base[0..4)
    ops += '\x02'; ops += uv(4); ops += "new!";                     // ADD
    ops += '\x03'; ops += 'Z'; ops += uv(9);                        // FILL
    ops += '\x04'; ops += sv(4); ops += uv(5);                      // COPYF file[4..9)
    const std::string p = srpatch(base, outS, ops);
    std::vector<uint8_t> pv(p.begin(), p.end()), bv(base.begin(), base.end()), ov;
    K::SrPatch sp;
    const bool parsed = K::srpatchParse(pv, &sp, &err);
    K::SrFileReader rd = [&](uint64_t off, uint64_t n, uint8_t* dst) {
      if (off + n > file.size()) return false;
      std::memcpy(dst, file.data() + off, static_cast<size_t>(n));
      return true;
    };
    check(parsed && sp.usesFile && K::srpatchApply(pv, sp, bv, rd, &ov, &err) && std::string(ov.begin(), ov.end()) == outS,
          "F7 SRPATCH1: COPY / ADD / FILL / COPYF rebuild the output and both hashes check");
    std::vector<uint8_t> wrong = bv;
    wrong[0] ^= 1;
    check(!K::srpatchApply(pv, sp, wrong, rd, &ov, &err), "F8 SRPATCH1: another base is refused (size + SHA-256)");
    std::vector<uint8_t> cut(pv.begin(), pv.end() - 2);
    check(!K::srpatchParse(cut, &sp, &err), "F9 SRPATCH1: a truncated patch is refused");
  }

  // ---- the fixture -------------------------------------------------------------------
  const std::string scrA = std::string("SCRA:") + std::string(200, 'a');
  const std::string scrB = std::string("SCRB:") + std::string(100, 'b');
  const std::string ui = bgfa({{"SCRA", scrA}, {"SCRB", scrB}});
  writeBin(disc / "DATA" / "ui.AST", ui);
  const uint32_t tocOff = static_cast<uint32_t>(uint8_t(ui[0x10]) | (uint8_t(ui[0x11]) << 8) | (uint8_t(ui[0x12]) << 16));
  const std::string tocSha = K::sha256Hex((ui.substr(0, 0x40) + ui.substr(tocOff)).data(), 0x40 + ui.size() - tocOff);
  // port feature custom_frontend: SCRA -> "SCRA:" + "PATCHED" + a's
  const std::string scrA2 = std::string("SCRA:PATCHED") + std::string(200, 'a');
  {
    std::string ops;
    ops += '\x01'; ops += sv(0); ops += uv(5);
    ops += '\x02'; ops += uv(7); ops += "PATCHED";
    ops += '\x01'; ops += sv(0); ops += uv(200);
    writeBin(root / "data" / "port_features" / "custom_frontend" / "SCRA.srpatch", srpatch(scrA, scrA2, ops));
    writeBin(root / "data" / "port_features" / "custom_frontend" / "feature.json",
             "{\"format\": 1, \"id\": \"custom_frontend\", \"name\": \"Custom front end\", \"option\": null,\n"
             " \"base\": {\"DATA/ui.AST\": {\"size\": " + std::to_string(ui.size()) + ", \"toc_sha256\": \"" + tocSha + "\"}},\n"
             " \"assets\": [{\"asset\": \"DATA/_workinprogress/custom_frontend/SCRA.ast\", \"base\": \"DATA/ui.AST:SCRA\", \"patch\": \"SCRA.srpatch\"}]}\n");
  }
  writeBin(mods / "alpha" / "mod.json",
           "{\"format\": 1, \"name\": \"Alpha Mod\", \"author\": \"A. Uthor\", \"version\": \"1.0\", "
           "\"description\": \"Lex model + a menu edit\", \"category\": \"characters\", \"priority\": 5, \"preview\": \"preview.png\"}");
  writeBin(mods / "alpha" / "preview.png", "PNG");
  writeBin(mods / "alpha" / "DATA" / "_workinprogress" / "alpha" / "X2_LEX.rx2", "ALPHA-LEX");
  writeBin(mods / "alpha" / "DATA" / "_workinprogress" / "alpha" / "SCRA.ast", "ALPHA-SCRA");
  writeBin(mods / "beta" / "DATA" / "_workinprogress" / "beta" / "x2_lex.RX2", "BETA-LEX");  // legacy (no mod.json)
  writeBin(mods / "beta" / "DATA" / "_workinprogress" / "beta" / "notes.zzz", "ignored by the engine");
  writeBin(mods / "broken" / "mod.json", "{\"format\": 1, \"name\": }");
  writeBin(mods / "needs" / "mod.json", "{\"format\": 1, \"requires\": [\"nothere\"]}");
  writeBin(mods / "custom_frontend" / "DATA" / "ui.AST", "OLD FORM A");
  writeBin(mods / "offbydefault" / "mod.json", "{\"format\": 1, \"enabled\": false}");
  writeBin(mods / "wronggame" / "mod.json", "{\"format\": 1, \"game\": \"SRX360-USA\"}");
  {
    std::string ops;
    ops += '\x01'; ops += sv(0); ops += uv(5);
    ops += '\x02'; ops += uv(4); ops += "MOD!";
    writeBin(mods / "patchmod" / "p.srpatch", srpatch(scrB, "SCRB:MOD!", ops));
    writeBin(mods / "patchmod" / "mod.json",
             "{\"format\": 1, \"patches\": [{\"asset\": \"DATA/_workinprogress/patchmod/SCRB.ast\", "
             "\"base\": \"DATA/ui.AST:SCRB\", \"patch\": \"p.srpatch\"}]}");
    writeBin(mods / "badpatch" / "p.srpatch", srpatch("not the base", "X", std::string("\x02") + uv(1) + "X"));
    writeBin(mods / "badpatch" / "mod.json",
             "{\"format\": 1, \"patches\": [{\"asset\": \"DATA/_workinprogress/badpatch/SCRB.ast\", "
             "\"base\": \"DATA/ui.AST:SCRB\", \"patch\": \"p.srpatch\"}]}");
  }
  writeBin(mods / "zipped.zip", zipStored({{"zipped/mod.json", "{\"format\": 1, \"name\": \"Zipped\"}"},
                                           {"zipped/DATA/_workinprogress/zipped/Z.lua", "-- lua"}}));
  writeBin(mods / "badzip.zip", "PK\x03\x04 this is not a zip");

  // ---- M: a player launch with no mods.json ---------------------------------------
  K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
  {
    const auto list = K::modMenuList();
    std::printf("%s", summary().c_str());
    check(!list.empty() && list[0].id == "alpha", "M1 default order: mod.json priority first (alpha, 5), then by id");
    const K::ModInfo* a = findMod(list, "alpha");
    check(a && a->status == K::ModStatus::Ok && a->mounted && a->name == "Alpha Mod" && a->author == "A. Uthor" &&
              a->version == "1.0" && a->category == "characters" && !a->preview.empty() && !a->legacy,
          "M2 alpha: manifest fields, preview resolved, mounted");
    const K::ModInfo* b = findMod(list, "beta");
    check(b && b->legacy && b->status == K::ModStatus::Ok && b->name == "beta", "M3 beta: a legacy mod (no mod.json) mounts");
    const K::ModInfo* br = findMod(list, "broken");
    check(br && br->status == K::ModStatus::BadManifest && br->reason.find("line 1") != std::string::npos && !br->mounted,
          "M4 broken mod.json: bad-manifest with the parser's position, not mounted");
    const K::ModInfo* nd = findMod(list, "needs");
    check(nd && nd->status == K::ModStatus::MissingDependency, "M5 requires a missing mod: missing-dependency");
    const K::ModInfo* cf = findMod(list, "custom_frontend");
    check(cf && cf->status == K::ModStatus::Reserved, "M6 an old mods/custom_frontend folder: reserved (the port feature wins)");
    const K::ModInfo* off = findMod(list, "offbydefault");
    check(off && !off->enabled && off->status == K::ModStatus::Disabled, "M7 mod.json enabled=false: found but off");
    const K::ModInfo* wg = findMod(list, "wronggame");
    check(wg && wg->status == K::ModStatus::WrongGame, "M8 game SRX360-USA on non-retail data: wrong-game");
    const K::ModInfo* pm = findMod(list, "patchmod");
    const K::ModInfo* bp = findMod(list, "badpatch");
    check(pm && pm->status == K::ModStatus::Ok && bp && bp->status == K::ModStatus::BadPatch,
          "M9 a mod's SRPATCH1 asset applies; a patch for other bytes: bad-patch");
    const K::ModInfo* z = findMod(list, "zipped");
    const K::ModInfo* bz = findMod(list, "badzip");
    check(z && z->isZip && z->status == K::ModStatus::Ok && z->name == "Zipped" && bz && bz->status == K::ModStatus::BadZip,
          "M10 a zip mod (single top folder) extracts and mounts; a damaged zip: bad-zip");
    check(!fs::exists(mods / "mods.json", ec), "M11 a launch without changes writes no mods.json");
    const auto feats = K::modMenuFeatures();
    check(feats.size() == 1 && feats[0].applied, "M12 the port feature is applied for a player launch");
  }
  {
    // the overlay serves the layers: the generated port asset, a mod's file, the zip's file
    const std::string discPath = K::contentRootDir() + "Superman Returns (USA)/";
    std::string gen;
    for (const auto& layer : K::modActiveLayers())
      if (layer.id == "custom_frontend" && !layer.roots.empty())
        gen = layer.roots[0] + "/DATA/_workinprogress/custom_frontend/SCRA.ast";
    check(!gen.empty() && readBin(fs::u8path(gen)) == scrA2,
          "M13 the port feature's generated asset = the patched disc entry");
    check(K::modOverlayResolveFile(discPath + "DATA/_workinprogress/custom_frontend/SCRA.ast").empty(),
          "M13b a conflicting lower-priority asset cannot be opened by its own loose path");
    // M13c (lane E, step 5 E2): a same-name cached generation WITHOUT its .complete marker (a boot killed mid-copy,
    // or a build from before the marker) is rebuilt. Before the fix the rename onto that non-empty folder failed on
    // every boot and the port feature fell back silently.
    {
      std::error_code ec2;
      std::string genRoot;
      for (const auto& layer : K::modActiveLayers())
        if (layer.id == "custom_frontend" && !layer.roots.empty()) genRoot = layer.roots[0];
      const fs::path marker = fs::u8path(genRoot) / ".complete";
      const bool hadMarker = !genRoot.empty() && fs::exists(marker, ec2);
      // Astra E2 r1 P3: the removal must have happened, or a still-complete cache satisfies every assertion below
      const bool removed = fs::remove(marker, ec2) && !ec2 && !fs::exists(marker, ec2);
      check(hadMarker && removed, "M13c precondition: the generation's .complete marker existed and is now removed");
      K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
      const auto f2 = K::modMenuFeatures();
      std::string gen2;
      for (const auto& layer : K::modActiveLayers())
        if (layer.id == "custom_frontend" && !layer.roots.empty())
          gen2 = layer.roots[0] + "/DATA/_workinprogress/custom_frontend/SCRA.ast";
      check(hadMarker && removed && f2.size() == 1 && f2[0].applied && fs::exists(marker, ec2) && !gen2.empty() &&
                readBin(fs::u8path(gen2)) == scrA2,
            "M13c a cached generation missing its .complete marker is rebuilt (the port feature still applies)");
      // M13d (Astra E2 r1 P1): another layer whose id extends this one ("<id>-x") keeps its generation, and no lock
      // file is left behind.
      const fs::path genParent = fs::u8path(genRoot).parent_path();   // <cache>/gen
      const fs::path other = genParent / "port-custom_frontend-x-0123456789abcdef";
      fs::create_directories(other, ec2);
      writeBin(other / ".complete", "ok\n");
      const bool removed2 = fs::remove(marker, ec2) && !ec2 && !fs::exists(marker, ec2);   // Astra E2 r2 P3
      K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
      bool lockLeft = false;
      for (const auto& e : fs::directory_iterator(genParent, ec2))
        if (e.path().extension() == ".lock") lockLeft = true;
      check(removed2 && fs::exists(other / ".complete", ec2) && fs::exists(marker, ec2) && !lockLeft,
            "M13d a layer whose id extends this one keeps its cache; the rebuild leaves no lock file");
      fs::remove_all(other, ec2);
#ifdef _WIN32
      // M13e (Astra E2 r2 P1): an unused older generation of THIS layer is collected; one another holder still uses
      // (its .use open) is kept. The test holds the old tag's .use open itself, as a second process would, with
      // exactly the production user's access and sharing (holdGenerationUse; Astra E2 r3: a stricter holder would
      // also stop a collector that wrongly allowed shared readers).
      const fs::path old1 = genParent / "port-custom_frontend-00000000000000aa";
      const fs::path old2 = genParent / "port-custom_frontend-00000000000000bb";
      fs::create_directories(old1, ec2);
      fs::create_directories(old2, ec2);
      writeBin(old1 / ".complete", "ok\n");
      writeBin(old2 / ".complete", "ok\n");
      HANDLE inUse = CreateFileW((genParent / "port-custom_frontend-00000000000000bb.use").wstring().c_str(),
                                 GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      // M13f (Astra E2 r3 P3): what a failed or killed generation leaves - a .use with no folder (it failed before
      // its temp folder) and a .lock (its process died) - is collected too, so failed cache keys cannot pile up.
      const fs::path old3use = genParent / "port-custom_frontend-00000000000000cc.use";
      const fs::path old3lock = genParent / "port-custom_frontend-00000000000000cc.lock";
      writeBin(old3use, "");
      writeBin(old3lock, "");
      // M13g (Astra E2 r4 P2): a generation whose marker cannot be removed (the test holds .complete open without
      // delete sharing) is kept WHOLE: its "-x" file must survive - never a partial folder that still claims to be
      // complete. Precondition (Astra E2 r5): the folder lists "-x" BEFORE .complete (the listing a recursive removal
      // walks), so a collector that does not invalidate the marker first removes "-x" whether it stops at its first
      // error or goes on.
      const fs::path old4 = genParent / "port-custom_frontend-00000000000000dd";
      fs::create_directories(old4, ec2);
      writeBin(old4 / ".complete", "ok\n");
      writeBin(old4 / "-x", "payload");
      HANDLE markHeld = CreateFileW((old4 / ".complete").wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      std::vector<std::string> old4Order;
      for (fs::directory_iterator it(old4, ec2), end; !ec2 && it != end; it.increment(ec2))
        old4Order.push_back(it->path().filename().string());
      const bool xFirst = old4Order.size() == 2 && old4Order[0] == "-x" && old4Order[1] == ".complete";
      // Astra E2 r4 P3: every fixture exists before the boot, and "gone" means gone without a filesystem error
      auto present = [](const fs::path& p) { std::error_code e; return fs::is_regular_file(p, e) && !e; };
      auto absent = [](const fs::path& p) { std::error_code e; return !fs::exists(p, e) && !e; };
      const bool fixtures = present(old1 / ".complete") && present(old2 / ".complete") && present(old3use) &&
                            present(old3lock) && present(old4 / "-x") && inUse != INVALID_HANDLE_VALUE &&
                            markHeld != INVALID_HANDLE_VALUE;
      const bool removed3 = fs::remove(marker, ec2) && !ec2;
      K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
      const bool oldGone = absent(old1) && absent(genParent / "port-custom_frontend-00000000000000aa.use");
      const bool heldKept = present(old2 / ".complete");
      const bool wholeKept = present(old4 / "-x") && present(old4 / ".complete");
      if (inUse != INVALID_HANDLE_VALUE) CloseHandle(inUse);
      if (markHeld != INVALID_HANDLE_VALUE) CloseHandle(markHeld);
      check(fixtures && removed3 && oldGone && heldKept && fs::exists(marker, ec2),
            "M13e an unused older generation is collected; one whose .use another holder has open is kept");
      check(fixtures && removed3 && absent(old3use) && absent(old3lock),
            "M13f an orphan .use (no folder) and a dead generator's .lock of an older generation are collected");
      check(fixtures && xFirst && removed3 && wholeKept,
            "M13g a generation whose marker cannot be removed is kept whole (no partial folder claiming complete)");
      fs::remove_all(old2, ec2);
      fs::remove(genParent / "port-custom_frontend-00000000000000bb.use", ec2);
      fs::remove_all(old4, ec2);
      fs::remove(genParent / "port-custom_frontend-00000000000000dd.use", ec2);
#endif
    }
    const std::string pmf = K::modOverlayResolveFile(discPath + "DATA/_workinprogress/patchmod/SCRB.ast");
    check(!pmf.empty() && readBin(fs::u8path(pmf)) == "SCRB:MOD!", "M14 a mod's patched asset is served");
    const std::string zf = K::modOverlayResolveFile(discPath + "DATA/_workinprogress/zipped/Z.lua");
    check(!zf.empty() && readBin(fs::u8path(zf)) == "-- lua", "M15 a zip mod's file is served from its extracted copy");
    // conflicts: alpha (top) vs beta: the same stem X2_LEX (case-insensitive) -> beta's hidden
    const auto conf = K::modMenuConflicts();
    bool lex = false, scra = false;
    for (const auto& c : conf) {
      lex = lex || (c.what.find("x2_lex") != std::string::npos && c.winnerId == "alpha" && c.loserId == "beta");
      scra = scra || (c.what.find("SCRA") != std::string::npos && c.winnerId == "alpha" && c.loserId == "custom_frontend");
    }
    check(lex, "M16 the same asset stem in two mods: a conflict, the higher mod (alpha) wins");
    check(scra, "M17 a mod's SCRA.ast vs the port feature's: the mod wins (mods sit above port features)");
    std::vector<K::ModDirEntry> listing;
    K::modOverlayMergeDir(discPath + "DATA/_workinprogress/beta", listing);
    bool sawLex = false, sawNotes = false;
    for (const auto& e : listing) { sawLex = sawLex || K::modLower(e.name) == "x2_lex.rx2"; sawNotes = sawNotes || e.name == "notes.zzz"; }
    check(!sawLex && sawNotes, "M18 the overlay hides the losing file from the game's listing (other files stay)");
    std::vector<K::ModDirEntry> top;
    K::modOverlayMergeDir(discPath + "DATA/_workinprogress", top);
    int dirs = 0;
    for (const auto& e : top) dirs += e.isDir ? 1 : 0;
    check(dirs >= 5, "M19 DATA/_workinprogress lists every layer's subfolder (alpha, beta, custom_frontend, patchmod, zipped)");
  }

  // ---- menu: SR_Mod* commands, mods.json, restart -----------------------------------
  {
    std::vector<K::ModMenuCall> calls;
    check(!K::modMenuCommand("SR_Quit", "", &calls), "N1 a non-SR_Mod command is not the menu's");
    check(K::modMenuCommand("SR_ModList", "", &calls) && !calls.empty() && calls.front().method == "SR_ModBegin" &&
              calls.back().method == "SR_ModEnd",
          "N2 SR_ModList: SR_ModBegin ... SR_ModEnd");
    int items = 0, conflicts = 0;
    bool alphaName = false;
    for (const auto& c : calls) {
      items += c.method == "SR_ModItem";
      conflicts += c.method == "SR_ModConflict";
      alphaName = alphaName || (c.method == "SR_ModText" && c.args.size() == 3 && c.args[0].i == 0 &&
                                c.args[1].s == "name" && c.args[2].s == "Alpha Mod");
      if (c.args.size() > 3) items = -1000;
    }
    check(items == calls.front().args[0].i && conflicts >= 2 && alphaName,
          "N3 one SR_ModItem per mod, SR_ModText fields (index 0 = alpha's name), conflicts listed, <= 3 args per call");
    check(!K::modMenuRestartRequired(), "N4 nothing changed: no restart required");
    calls.clear();
    K::modMenuCommand("SR_ModSet", "beta 0", &calls);
    const std::string mj = readBin(mods / "mods.json");
    check(mj.find("\"disabled\": [") != std::string::npos && mj.find("\"beta\"") != std::string::npos &&
              K::modMenuRestartRequired() && calls.front().method == "SR_ModBegin" && (calls.front().args[1].i & 1),
          "N5 SR_ModSet beta 0: mods.json saved, restart required (state bit 0), the list is re-sent");
    auto indexOf = [](const std::string& id) {
      const auto l = K::modMenuList();
      for (size_t i = 0; i < l.size(); ++i) if (l[i].id == id) return static_cast<int>(i);
      return -1;
    };
    const int before = indexOf("beta");
    calls.clear();
    K::modMenuCommand("SR_ModMove", "beta -1", &calls);
    check(before > 0 && indexOf("beta") == before - 1, "N6 SR_ModMove beta -1: beta moves up one place");
    static int spawned = 0;
    K::modMenuSetRestartHookForTest([](std::string*) { ++spawned; return true; });
    calls.clear();
    K::modMenuCommand("SR_ModRestart", "", &calls);
    check(spawned == 1 && !calls.empty() && calls[0].method == "SR_ModRestarting" && calls[0].args[0].i == 1,
          "N7 SR_ModRestart: the relaunch hook runs, SR_ModRestarting(1)");
    K::modMenuSetRestartHookForTest(nullptr);
    // the next boot reads mods.json
    K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
    const auto l3 = K::modMenuList();
    const K::ModInfo* b3 = findMod(l3, "beta");
    check(b3 && !b3->enabled && b3->status == K::ModStatus::Disabled && !K::modMenuRestartRequired(),
          "N8 the next boot reads mods.json: beta off, no restart required");
    std::vector<K::ModDirEntry> listing;
    K::modOverlayMergeDir(K::contentRootDir() + "Superman Returns (USA)/DATA/_workinprogress/beta", listing);
    check(listing.empty(), "N9 a switched-off mod's files are not served");
  }

  // Failed saves must not change the displayed state or report a pending restart.
  {
    K::modOverlayResetForTestEnv(envFor(root, "@player", "0", true));
    const std::string saved = readBin(mods / "mods.json");
    const auto before = K::modMenuList();
    const K::ModInfo* beta = findMod(before, "beta");
    const bool wasEnabled = beta && beta->enabled;
    check(beta && !K::modMenuSetEnabled("beta", !wasEnabled) &&
              findMod(K::modMenuList(), "beta")->enabled == wasEnabled &&
              !K::modMenuRestartRequired() && readBin(mods / "mods.json") == saved,
          "N10 a read-only automated launch refuses a toggle without changing state");
    check(!K::modMenuMove(before[1].id, -1) && K::modMenuList()[0].id == before[0].id &&
              !K::modMenuRestartRequired() && readBin(mods / "mods.json") == saved,
          "N11 a read-only automated launch refuses reordering without changing state");
    K::modOverlayResetForTestEnv(envFor(root, nullptr, "0", false));
    // A directory at the temporary-file path deterministically simulates a failed save.
    fs::create_directory(mods / "mods.json.tmp", ec);
    const auto diskBefore = K::modMenuList();
    const bool betaBefore = findMod(diskBefore, "beta")->enabled;
    check(!K::modMenuSetEnabled("beta", !betaBefore) &&
              findMod(K::modMenuList(), "beta")->enabled == betaBefore &&
              !K::modMenuRestartRequired() && readBin(mods / "mods.json") == saved,
          "N12 a failed disk save rolls back a toggle");
    check(!K::modMenuMove(diskBefore[1].id, -1) && K::modMenuList()[0].id == diskBefore[0].id &&
              !K::modMenuRestartRequired() && readBin(mods / "mods.json") == saved,
          "N13 a failed disk save rolls back reordering");
    fs::remove(mods / "mods.json.tmp", ec);
  }

  // Independent character packs add entries; they do not compete for X2_SUPERMAN.
  {
    const std::string aEntity = K::modAssetPrefix("hero_a") + "ORIGINAL_ENT";
    const std::string bEntity = K::modAssetPrefix("hero_b") + "ORIGINAL_ENT";
    for (const auto& [id, entity] : std::vector<std::pair<std::string, std::string>>{
             {"hero_a", aEntity}, {"hero_b", bEntity}}) {
      writeBin(mods / id / "mod.json", "{\"costumes\": [{\"id\": \"original\", \"name\": \"" + id +
               "\", \"entity\": \"" + entity + "\"}]}");
      writeBin(mods / id / "DATA" / "_workinprogress" / id / (entity + ".xml"), "<entity/>\0");
      writeBin(mods / id / "DATA" / "_workinprogress" / id / (K::modAssetPrefix(id) + "BODY.rx2"), id);
    }
    writeBin(mods / "badskin" / "mod.json",
             "{\"costumes\": [{\"id\": \"original\", \"name\": \"Bad skin\", \"entity\": \"SupermanEnt\"}]}");
    K::modOverlayResetForTestEnv(envFor(root, "hero_b;hero_a;badskin", "0", true));
    const auto cs = K::modCostumeList();
    check(cs.size() == 2 && cs[0].id == "hero_a/original" && cs[1].id == "hero_b/original" &&
              cs[0].slot == 5 && cs[1].slot == 6 && cs[0].entity != cs[1].entity,
          "C1 two enabled character packs add distinct entries after all retail slots");
    check(K::modMenuConflicts().empty(), "C2 independent character assets coexist without replacement conflicts");
    check(findMod(K::modMenuList(), "badskin")->status == K::ModStatus::BadManifest,
          "C3 an additive costume cannot replace SupermanEnt");
    K::modOverlayResetForTestEnv(envFor(root, "hero_a;hero_b", "0", true));
    const auto reordered = K::modCostumeList();
    check(reordered.size() == 2 && reordered[0].id == cs[0].id && reordered[0].slot == cs[0].slot &&
              reordered[1].id == cs[1].id && reordered[1].slot == cs[1].slot,
          "C4 costume identities and slot assignment do not depend on mod priority");
    K::modOverlayResetForTestEnv(envFor(root, "hero_b", "0", true));
    const auto alone = K::modCostumeList();
    check(alone.size() == 1 && alone[0].id == "hero_b/original" && alone[0].slot == 5,
          "C5 disabling one pack removes only its entries; stable ids survive slot reassignment");
    K::modOverlayResetForTestEnv(envFor(root, nullptr, "0", true));
    check(K::modCostumeList().empty(), "C6 natural launches have no mod costume entries");
  }

  // ---- migration from enabled.txt -----------------------------------------------------
  {
    fs::remove(mods / "mods.json", ec);
    writeBin(mods / "enabled.txt", "# mine\r\nbeta\r\nzipped  # second\r\n");
    K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, false));
    const auto l = K::modMenuList();
    const K::ModInfo* a = findMod(l, "alpha");
    check(l.size() >= 2 && l[0].id == "beta" && l[1].id == "zipped" && a && !a->enabled,
          "G1 migration: enabled.txt's mods on top and on, every other mod off");
    check(fs::exists(mods / "mods.json", ec) && !fs::exists(mods / "enabled.txt", ec) &&
              fs::exists(mods / "enabled.txt.migrated", ec),
          "G2 migration writes mods.json and renames enabled.txt -> enabled.txt.migrated");
  }

  // ---- automated runs ---------------------------------------------------------------
  {
    const std::string before = readBin(mods / "mods.json");
    K::modOverlayResetForTestEnv(envFor(root, nullptr, nullptr, true));
    check(K::modActiveLayers().empty() && !K::modOverlayActive(), "A1 SR_INSTANCE_ID, no SR_MODS: nothing mounted (natural)");
    K::modOverlayResetForTestEnv(envFor(root, nullptr, "1", true));
    check(K::modActiveLayers().size() == 1 && K::modActiveLayers()[0].port, "A2 SR_PORT_FEATURES=1 in a roll: only the port feature");
    K::modOverlayResetForTestEnv(envFor(root, "custom_frontend", nullptr, true));
    check(K::modActiveLayers().size() == 1 && K::modActiveLayers()[0].id == "custom_frontend",
          "A3 SR_MODS=custom_frontend (iter-463 rolls) turns the port feature on");
    K::modOverlayResetForTestEnv(envFor(root, "alpha;beta", "0", true));
    check(K::modActiveLayers().size() == 2 && K::modActiveLayers()[0].id == "alpha" && K::modOverlayEnabled().size() == 2,
          "A4 SR_MODS=alpha;beta + SR_PORT_FEATURES=0: exactly those mods, in that order");
    K::modOverlayResetForTestEnv(envFor(root, "@player", "0", true));
    check(!K::modActiveLayers().empty() && readBin(mods / "mods.json") == before,
          "A5 SR_MODS=@player in a roll: the player's state is used, mods.json is not written");
    std::string why;
    check(!K::modMenuRestart(&why) && why.find("automated") != std::string::npos, "A6 restart is refused in automated runs");
    K::modOverlayResetForTestEnv(envFor(root, "0", nullptr, false));
    check(K::modActiveLayers().empty(), "A7 SR_MODS=0 (the non-roll launchers): no mods and no port features");
  }

  // ---- the real disc: the fingerprint ------------------------------------------------
  {
    const std::string realDisc = K::contentRootDir() + "Superman Returns (USA)";
    const std::string fp = K::modDataFingerprint(realDisc);
    std::printf("       fingerprint of the repo's disc: %s\n", fp.c_str());
    check(fp == "SRX360-USA:fe43807d8d15db16", "R1 the USA retail data fingerprint (tools/srmods/srmods.py fingerprint)");
    std::vector<uint8_t> e;
    std::string err;
    check(K::modReadTopEntry(realDisc + "/DATA/ui.AST", "ScrPressStart", &e, &err) && e.size() == 445696 &&
              std::memcmp(e.data(), "BGFA1.05", 8) == 0,
          "R2 modReadTopEntry(ui.AST, ScrPressStart) = the 445,696-byte movie container");
    std::vector<uint8_t> bundle, script, attrs, combined;
    const bool loaded = K::modReadTopEntry(realDisc + "/DATA/objtiv.AST", "SUPERMENACTORWORLDFLOW", &bundle, &err) &&
                        K::modReadNestedEntry(bundle, "SUPERMENACTORWORLDFLOW_SCRIPT", &script, &err) &&
                        K::modReadNestedEntry(script, "SUPERMENACTORWORLDFLOWATTRIBUTES", &attrs, &err);
    check(loaded && attrs.size() == 644, "R3 required retail costume attribute document read through nested containers");
    {
      std::vector<uint8_t> other, preloads, cooked;
      const bool read = K::modReadTopEntry(realDisc + "/DATA/baseworl.AST", "BASEWORLDFLOW_OTHER", &other, &err) &&
                        K::modReadNestedEntry(other, "BASEWORLDFLOWPRELOADS", &preloads, &err);
      check(read, "P1 required retail base preload document is available (missing input cannot pass)");
      K::ModCostumeInfo own;
      own.source = "hero_a";
      own.animationProject = K::modAssetPrefix(own.source) + "Anims";
      const bool ok = read && K::modComposeCostumePreloads(preloads, {own, own}, &cooked, &err);
      const std::string text(cooked.begin(), cooked.end());
      const size_t first = text.find(own.animationProject);
      check(ok && first != std::string::npos && text.find(own.animationProject, first + 1) == std::string::npos &&
                text.find("SupermanAnims") != std::string::npos && text.find("async=\"0\"") != std::string::npos &&
                !cooked.empty() && cooked.back() == 0,
            "P2 own project is appended once without removing retail animation/sound preloads");
      auto alias = own;
      alias.animationProject = K::modLower(alias.animationProject);
      check(read && !K::modComposeCostumePreloads(preloads, {own, alias}, &cooked, &err),
            "P3 case-ambiguous project/resource identity is refused");
      alias.animationProject = "SupermanAnims";
      check(read && !K::modComposeCostumePreloads(preloads, {alias}, &cooked, &err), "P4 retail project replacement refused");
      bool malformed = read;
      if (read) {
        // Truncation and impossible element-vector count. Keep the successful
        // output as a canary: failures must not publish partially written data.
        const auto prior = cooked;
        for (size_t length : {size_t(0), size_t(43), preloads.size() / 2}) {
          const std::vector<uint8_t> cut(preloads.begin(), preloads.begin() + length);
          malformed = malformed && !K::modComposeCostumePreloads(cut, {own}, &cooked, &err) && cooked == prior;
        }
        auto corrupt = preloads;
        corrupt[16] = corrupt[17] = 0xFF;
        malformed = malformed && !K::modComposeCostumePreloads(corrupt, {own}, &cooked, &err) && cooked == prior;
      }
      check(malformed, "P5 malformed preload structures refuse without altering the caller's output");
      check(!K::modComposeCostumePreloads(std::vector<uint8_t>{'<','x','/','>'}, {own}, &cooked, &err),
            "P6 unsupported text preload bases fail closed instead of dropping their contents");
    }
    std::vector<K::ModCostumeInfo> additions = {
        {"hero_a/original", "original", "Hero A", "hero_a", K::modAssetPrefix("hero_a") + "ENT", 5},
        {"hero_b/original", "original", "Hero B", "hero_b", K::modAssetPrefix("hero_b") + "ENT", 6}};
    const bool composed = loaded && K::modComposeCostumeAttributes(attrs, additions, &combined, &err);
    const std::string xml(combined.begin(), combined.end());
    check(composed && xml.find("value=\"SupermanEnt\"") != std::string::npos &&
              xml.find("value=\"SupermanGrayEnt\"") != std::string::npos &&
              xml.find("value=\"SupermanClassicEnt\"") != std::string::npos &&
              xml.find("value=\"SupermanCrazyEnt\"") != std::string::npos &&
              xml.find("key=\"Costume5\"") != std::string::npos && xml.find("key=\"Costume6\"") != std::string::npos &&
              xml.back() == '\0', "R4 one table adds both packs, preserves four retail suits and terminates text");
    auto bad = additions;
    bad[1].slot = 5;
    check(loaded && !K::modComposeCostumeAttributes(attrs, bad, &combined, &err), "R5 duplicate slots refused, no retail replacement");
    bad = additions;
    bad[0].slot = 1;
    check(loaded && !K::modComposeCostumeAttributes(attrs, bad, &combined, &err), "R6 retail slots refused");
    check(!K::modComposeCostumeAttributes(std::vector<uint8_t>(16), additions, &combined, &err) &&
              !K::modReadNestedEntry(std::vector<uint8_t>(16), "anything", &e, &err), "R7 truncated XML and container refused");
    const bool selected = loaded && K::modComposeCostumeAttributes(attrs, additions, &combined, &err, 6);
    const std::string selectedXml(combined.begin(), combined.end());
    check(selected && selectedXml.find("key=\"SRWornCostume\" type=\"Integer\" category=\"0\" constant=\"0\" value=\"6\"") != std::string::npos &&
              selectedXml.find("key=\"SetCostumeNum\" type=\"Integer\" category=\"0\" constant=\"0\" value=\"0\"") != std::string::npos &&
              selectedXml.find("key=\"SRCostumeFamily6\" type=\"String\" category=\"0\" constant=\"1\" value=\"retail\"") != std::string::npos &&
              selectedXml.find("key=\"SRActiveFamily\" type=\"String\" category=\"0\" constant=\"1\" value=\"retail\"") != std::string::npos &&
              selectedXml.find("key=\"Costume1\"") != std::string::npos,
          "R8 the worn suit is content (SRWornCostume), never a pending request; every suit names its animation family");
    check(loaded && !K::modComposeCostumeAttributes(attrs, additions, &combined, &err, 7),
          "R9 startup request cannot refer to an absent costume slot");
    if (loaded) {
      std::vector<uint8_t> actorLua, guarded;
      const bool actorRead = K::modReadNestedEntry(script, "SUPERMENACTORWORLDFLOWFSM_S_60265281", &actorLua, &err);
      const bool switcher = actorRead && K::modComposeCostumeSwitchLua(actorLua, "SUPERMENACTORWORLDFLOWFSM_S_60265281", &guarded, &err);
      const std::string guardedText(guarded.begin(), guarded.end());
      check(switcher && guardedText.find("if (supermanID == -1 or supermanID == 0) then return end") <
                guardedText.find("SetDASValue(\"SetCostumeNum\", 0)") &&
                guardedText.find("GetDASValue(\"SRCostumeFamily\" .. costumeNum) == GetDASValue(\"SRActiveFamily\")") <
                guardedText.find("ReplaceEntityVisRep(supermanID, newCostume);") &&
                guardedText.find("GetDASValue(\"SRCostumeChar\" .. costumeNum) == 1") <
                guardedText.find("srSpawnCharacter(supermanID, newCostume)") &&
                guardedText.find("SpawnEntityActive(entity, px, py, pz, ox, oy, oz)") != std::string::npos &&
                guardedText.find("SpawnEntityActive", guardedText.find("SpawnEntityActive(entity") + 1) == std::string::npos,
            "R28 switcher: waits for Superman; the retail VisRep switch only within this session's animation family; "
            "a spawn only for a whole character on its own project (SRCostumeChar)");
      auto foreign = actorLua;
      foreign.insert(foreign.end(), actorLua.begin(), actorLua.end());
      check(actorRead && !K::modComposeCostumeSwitchLua(foreign, "SUPERMENACTORWORLDFLOWFSM_S_60265281", &guarded, &err),
            "R29 an actor costume state that is not exactly the retail one is refused");
      std::vector<uint8_t> baseScripts;
      const bool scriptsLoaded = K::modReadTopEntry(realDisc + "/DATA/baseworl.AST", "BASEWORLDFLOW_SCRIPT", &baseScripts, &err);
      check(scriptsLoaded && !baseScripts.empty(), "R19 required retail world-flow Lua read for the startup bridge");
      if (scriptsLoaded) writeBin(disc / "DATA/baseworl.AST", bgfa({{"BASEWORLDFLOW_SCRIPT", std::string(baseScripts.begin(), baseScripts.end())}}));
      for (const char* entry : {"BASEWORLDFLOWFSM_S_7E412353", "BASEWORLDFLOWFSM_S_4982930A"}) {
        std::vector<uint8_t> lua, changed;
        const bool read = scriptsLoaded && K::modReadNestedEntry(baseScripts, entry, &lua, &err);
        const bool cooked = read && K::modComposeCostumeSwitchLua(lua, entry, &changed, &err);
        const std::string text(changed.begin(), changed.end());
        const bool keepsWorn = std::string(entry) == "BASEWORLDFLOWFSM_S_7E412353"
            ? text.find("\"SetCostumeNum\", 1)") == std::string::npos
            : text.find("ReplaceEntityVisRep(supermanID, srSuit);") != std::string::npos &&
              text.find("\"Costume\" .. srWorn)") != std::string::npos &&
              text.find("IsWorldFlowRunning(\"SupermenActorWorldFlow\")") < text.find("GetWorldFlowDASValue") &&
              text.find("ReplaceEntityVisRep(supermanID, \"SupermanEnt\")") == std::string::npos &&
              text.find("ReplaceEntityVisRep(supermanID, \"\")") != std::string::npos;
        check(cooked && keepsWorn && text.size() > lua.size() / 2 &&
                  std::find(changed.begin(), changed.end(), uint8_t(0)) == changed.end(),
              (std::string("R20 world-flow Lua keeps the worn suit instead of the retail reset: ") + entry).c_str());
        auto duplicate = lua;
        duplicate.insert(duplicate.end(), lua.begin(), lua.end());
        check(read && !K::modComposeCostumeSwitchLua(duplicate, entry, &changed, &err), "R21 ambiguous Lua refused");
      }
      check(!K::modComposeCostumeSwitchLua(std::vector<uint8_t>{'x'}, "unknown", &combined, &err), "R22 unexpected Lua refused");
      writeBin(disc / "DATA" / "objtiv.AST", bgfa({{"SUPERMENACTORWORLDFLOW", bgfa({{"SUPERMENACTORWORLDFLOW_SCRIPT",
               bgfa({{"SUPERMENACTORWORLDFLOWATTRIBUTES", std::string(attrs.begin(), attrs.end())},
                     {"SUPERMENACTORWORLDFLOWFSM_S_60265281", std::string(actorLua.begin(), actorLua.end())}})}})}}));
      writeBin(mods / "mods.json", "{\"order\":[\"hero_a\",\"hero_b\"],\"disabled\":[],\"costume\":\"\"}");
      const auto player = envFor(root, nullptr, "0", false);
      K::modOverlayResetForTestEnv(player);
      const auto tablePath = [](const auto& layers) {
        for (const auto& layer : layers) if (layer.id == "_costume_registry")
          return fs::u8path(layer.roots.front()) / "DATA/_workinprogress/_costume_registry/SUPERMENACTORWORLDFLOWATTRIBUTES.das";
        return fs::path();
      };
      const fs::path bootTable = tablePath(K::modActiveLayers());
      const std::string bootBytes = readBin(bootTable);
#ifdef _WIN32
      // Exercise real generated-asset writes AND readback, not a path helper.
      // Windows requires BOTH LongPathsEnabled=1 and the executable manifest.
      {
        HMODULE registryApi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto readRegistry = registryApi ? reinterpret_cast<decltype(&RegGetValueW)>(
            GetProcAddress(registryApi, "RegGetValueW")) : nullptr;
        DWORD longPaths = 0, policyBytes = sizeof longPaths;
        const bool policyEnabled = readRegistry && readRegistry(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"LongPathsEnabled",
            RRF_RT_REG_DWORD, nullptr, &longPaths, &policyBytes) == ERROR_SUCCESS && longPaths == 1;
        if (registryApi) FreeLibrary(registryApi);
        check(policyEnabled, "LP1 long-path integration regression requires OS LongPathsEnabled=1");
        if (policyEnabled) {
          const fs::path unicodeInstall = root / fs::path(L"Superman Returns \u8def\u5f84 \u00e9");
          const size_t wantedRootLength = 140;
          const bool rootFits = unicodeInstall.native().size() + 1 < wantedRootLength;
          check(rootFits, "LP2 bounded Unicode/space fixture can reach the long-path boundary");
          if (rootFits) {
            const std::string priorSelection = readBin(mods / "mods.json");
            writeBin(mods / "mods.json", "{\"order\":[\"hero_a\",\"hero_b\"],\"disabled\":[],\"costume\":\"hero_b/original\"}");
            K::modOverlayResetForTestEnv(player);
            const fs::path shortTable = tablePath(K::modActiveLayers());
            const std::string shortBytes = readBin(shortTable);
            const fs::path longRoot = unicodeInstall /
                std::string(wantedRootLength - unicodeInstall.native().size() - 1, 'L');
            const fs::path longDisc = longRoot / "Superman Returns (USA)";
            const fs::path longMods = longRoot / "mods";
            writeBin(longDisc / "DATA/objtiv.AST", readBin(disc / "DATA/objtiv.AST"));
            writeBin(longDisc / "DATA/baseworl.AST", readBin(disc / "DATA/baseworl.AST"));
            fs::create_directories(longMods, ec);
            for (const char* id : {"hero_a", "hero_b"})
              fs::copy(mods / id, longMods / id, fs::copy_options::recursive, ec);
            writeBin(longMods / "mods.json", readBin(mods / "mods.json"));
            const char* assetNames[] = {"SUPERMENACTORWORLDFLOWATTRIBUTES.das",
                "SUPERMENACTORWORLDFLOWFSM_S_60265281.lua", "BASEWORLDFLOWFSM_S_7E412353.lua",
                "BASEWORLDFLOWFSM_S_4982930A.lua"};
            std::vector<std::string> expectedAssets;
            bool sourceComplete = !shortTable.empty() && !shortBytes.empty();
            for (const char* name : assetNames) {
              expectedAssets.push_back(readBin(shortTable.parent_path() / name));
              sourceComplete = sourceComplete && !expectedAssets.back().empty();
            }
            check(sourceComplete, "LP3 required short-path baseline contains all four real generated assets");
            const fs::path expectedTable = longRoot / shortTable.lexically_relative(root);
            check(expectedTable.native().size() > MAX_PATH &&
                      (expectedTable.parent_path() / assetNames[1]).native().size() > MAX_PATH,
                  "LP4 final generated paths, not merely .tmp names, exceed MAX_PATH");
            const auto longPlayer = envFor(longRoot, nullptr, "0", false);
            K::modOverlayResetForTestEnv(longPlayer);
            const fs::path actualTable = tablePath(K::modActiveLayers());
            const std::string identityDir = K::modPathU8(shortTable.parent_path().parent_path().parent_path().parent_path().filename());
            const bool fullSha = identityDir.size() == 73 && identityDir.rfind("costumes-", 0) == 0 &&
                std::all_of(identityDir.begin() + 9, identityDir.end(), [](char c) {
                  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                });
            check(K::modCostumeRegistryReady() && fullSha && actualTable == expectedTable,
                  "LP5 Unicode/space long install publishes registry with unchanged full SHA identity");
            bool exact = sourceComplete;
            bool noTemps = true;
            for (size_t i = 0; i < std::size(assetNames); ++i) {
              const fs::path path = expectedTable.parent_path() / assetNames[i];
              exact = exact && readBin(path) == expectedAssets[i];
              fs::path tmp = path;
              tmp += ".tmp";
              const bool hasTemp = fs::exists(tmp, ec);
              noTemps = noTemps && !hasTemp && !ec;
            }
            check(exact && noTemps, "LP6 all four generated long-path assets read back byte-exact with no temporary residue");
            K::modOverlayResetForTestEnv(longPlayer);
            check(K::modCostumeRegistryReady() && tablePath(K::modActiveLayers()) == expectedTable &&
                      readBin(expectedTable) == shortBytes,
                  "LP7 repeat long-path boot replaces cached assets and preserves their bytes");
            writeBin(mods / "mods.json", priorSelection);
            K::modOverlayResetForTestEnv(player);
          }
        }
      }
#endif
      K::modMenuResetForTest();
#ifdef _WIN32
      const char* oldManager = std::getenv("SR_MOD_MANAGER");
      const bool hadManager = oldManager != nullptr;
      const std::string savedManager = oldManager ? oldManager : "";
      _putenv_s("SR_MOD_MANAGER", "1");
#endif
      K::modMenuSetCatalog(K::modMenuMakeModcoreCatalog());
      const auto menu = [](const std::string& arg) {
        std::vector<K::ModReply> replies;
        K::modBridgeDispatchEx("SR_Menu", arg, &replies);
        return K::hostPageDecodeForTest(replies);
      };
      auto page = menu("open costumes FE");
      check(page.labels.size() == 3 && page.labels[0] == "RETAIL DEFAULT (SELECTED)" &&
                page.labels[1] == "HERO_A" && page.labels[2] == "HERO_B" &&
                K::modMenuCostumeCount() == 3 && page.body.find("restart") != std::string::npos,
            "R23 managed picker adds both character packs and keeps a retail default; no false WEARING claim");
      page = menu("back costumes:0");
      check(page.page == "modextras", "R24 B returns from the managed picker to mod options");
      page = menu("open costumes:999 FE");
      check(page.page == "costumes:0" && page.labels.size() == 3, "R25 stale costume page clamps to the populated catalog");
      check(K::modCostumeRegistryReady() && K::modCostumeSelectForRestart("HERO_B/ORIGINAL") &&
                K::modCostumeSelected() == "hero_b/original" && K::modMenuRestartRequired() &&
                readBin(mods / "mods.json").find("\"costume\": \"hero_b/original\"") != std::string::npos &&
                readBin(bootTable) == bootBytes,
            "R10 picker saves a stable id; immutable current-boot content is not changed");
      K::modOverlayResetForTestEnv(player);
      std::string selectedTable = readBin(tablePath(K::modActiveLayers()));
      check(K::modCostumeSelected() == "hero_b/original" && !K::modMenuRestartRequired() &&
                selectedTable.find("constant=\"0\" value=\"6\"") != std::string::npos,
            "R11 next boot resolves the saved id to the current DAS slot");
      page = menu("open costumes FE");
      check(page.labels[2] == "HERO_B (SELECTED)", "R26 managed picker displays the saved stable id");
      check(K::modMenuSetEnabled("hero_a", false), "R12 another pack can be disabled without losing selection");
      K::modOverlayResetForTestEnv(player);
      selectedTable = readBin(tablePath(K::modActiveLayers()));
      check(K::modCostumeSelected() == "hero_b/original" &&
                selectedTable.find("constant=\"0\" value=\"5\"") != std::string::npos,
            "R13 selection survives numeric slot reassignment after a different pack is disabled");
      fs::create_directory(mods / "mods.json.tmp", ec);
      const std::string stateBefore = readBin(mods / "mods.json");
      check(!K::modCostumeSelectForRestart("") && K::modCostumeSelected() == "hero_b/original" &&
                readBin(mods / "mods.json") == stateBefore && !K::modMenuRestartRequired(),
            "R14 failed selection save rolls back both state and pending restart");
      fs::remove(mods / "mods.json.tmp", ec);
      check(!K::modCostumeSelectForRestart("missing/suit") && !K::modCostumeSelectForRestart("hero_a/original"),
            "R15 absent and disabled packs cannot be selected");
      check(K::modCostumeSelectForRestart(""), "R16 retail default can be restored without replacing retail entries");
      K::modOverlayResetForTestEnv(envFor(root, "@player", "0", true));
      const std::string protectedState = readBin(mods / "mods.json");
      page = menu("press costumes:0 1");
      check(page.page == "modcostumeerror" && page.body.find("No suit was changed") != std::string::npos &&
                readBin(mods / "mods.json") == protectedState,
            "R27 refused UI selection is visible and cannot alter automated player state");
      check(!K::modCostumeSelectForRestart("hero_b/original") && readBin(mods / "mods.json") == protectedState,
            "R17 automated runs cannot write the player's costume selection");
      auto absent = envFor(root, "hero_a", "0", true);
      absent.startupCostume = "hero_b/original";
      K::modOverlayResetForTestEnv(absent);
      check(readBin(tablePath(K::modActiveLayers())).find("key=\"SRWornCostume\" type=\"Integer\" category=\"0\" constant=\"0\" value=\"0\"") != std::string::npos,
            "R18 removing the worn pack falls back to the retail default, not a different mod's numeric slot");

      // ---- own animation banks (iter-465 asset contract): families and the patched baseworl.AST
      {
        const std::string realBaseText = readBin(fs::u8path(realDisc) / "DATA/baseworl.AST");
        const std::vector<uint8_t> realBase(realBaseText.begin(), realBaseText.end());
        std::vector<uint8_t> anim, meleeAir, superspeed, flight, patched;
        const bool banksRead = K::modReadNestedEntry(realBase, "BASEWORLDFLOW_ANIM", &anim, &err) &&
                               K::modReadNestedEntry(anim, "SUPERMAN_MELEEAIR", &meleeAir, &err) &&
                               K::modReadNestedEntry(anim, "SUPERMAN_SUPERSPEED_ATTACKS", &superspeed, &err) &&
                               K::modReadNestedEntry(anim, "SUPERMAN_FLIGHT", &flight, &err);
        check(banksRead && K::modValidSupermanBank(meleeAir, &err) && K::modValidSupermanBank(flight, &err) &&
                  superspeed.size() < meleeAir.size() && flight.size() > meleeAir.size(),
              "AB1 the retail SupermanAnims banks are read from the disc and pass the bank check");
        patched = realBase;
        std::vector<uint8_t> anim2, slot, flight2, scripts, scripts2;
        const bool patchedOk = banksRead && K::modPatchBaseworldBanks(&patched, {{"superman_meleeair", superspeed}}, &err) &&
                               K::modReadNestedEntry(patched, "BASEWORLDFLOW_ANIM", &anim2, &err) &&
                               K::modReadNestedEntry(anim2, "SUPERMAN_MELEEAIR", &slot, &err) &&
                               K::modReadNestedEntry(anim2, "SUPERMAN_FLIGHT", &flight2, &err) &&
                               K::modReadNestedEntry(realBase, "BASEWORLDFLOW_SCRIPT", &scripts, &err) &&
                               K::modReadNestedEntry(patched, "BASEWORLDFLOW_SCRIPT", &scripts2, &err);
        size_t changed = 0;
        for (size_t i = 0; patchedOk && i < patched.size(); ++i) changed += patched[i] != realBase[i];
        check(patchedOk && patched.size() == realBase.size() && slot == superspeed && flight2 == flight &&
                  scripts2 == scripts && changed > 0 && changed <= meleeAir.size() + 4,
              "AB2 an own bank goes into its retail slot in place: same file size, TOC size updated, nothing else moves");
        auto untouched = realBase;
        check(banksRead && !K::modPatchBaseworldBanks(&untouched, {{"SUPERMAN_MELEEAIR", flight}}, &err) &&
                  untouched == realBase && err.find("retail slot") != std::string::npos,
              "AB3 a bank larger than its retail slot is refused and the file is unchanged");
        const std::vector<uint8_t> garbage(64, 0x41);
        check(banksRead && !K::modPatchBaseworldBanks(&untouched, {{"SUPERMAN_FLIGHT", superspeed}, {"SUPERMAN_MELEEAIR", garbage}}, &err) &&
                  !K::modPatchBaseworldBanks(&untouched, {{"SUPERMAN_CAPE", superspeed}}, &err) &&
                  !K::modPatchBaseworldBanks(&untouched, {{"SUPERMAN_MELEEAIR", superspeed}, {"superman_meleeair", superspeed}}, &err) &&
                  untouched == realBase,
              "AB4 a non-bank, an unknown bank name or a repeated name refuses the whole set before any byte changes");

        // the fixture disc gains a BASEWORLDFLOW_ANIM with the retail MELEEAIR slot
        const fs::path fixtureBase = disc / "DATA/baseworl.AST";
        const std::string fixtureBefore = readBin(fixtureBase);
        writeBin(fixtureBase, bgfa({{"BASEWORLDFLOW_SCRIPT", std::string(scripts.begin(), scripts.end())},
                                    {"BASEWORLDFLOW_ANIM", bgfa({{"SUPERMAN_MELEEAIR", std::string(meleeAir.begin(), meleeAir.end())}})}}));
        const std::string cEntity = K::modAssetPrefix("hero_c") + "ORIGINAL_ENT";
        const std::string dEntity = K::modAssetPrefix("hero_d") + "ORIGINAL_ENT";
        writeBin(mods / "hero_c/mod.json", "{\"costumes\": [{\"id\": \"original\", \"name\": \"Hero C\", \"entity\": \"" + cEntity +
                 "\", \"animation_banks\": {\"SUPERMAN_MELEEAIR\": \"banks/meleeair.bin\"}}]}");
        writeBin(mods / "hero_c/banks/meleeair.bin", std::string(superspeed.begin(), superspeed.end()));
        writeBin(mods / "hero_c/DATA/_workinprogress/hero_c" / (cEntity + ".xml"), std::string("<entity/>\0", 10));
        writeBin(mods / "hero_d/mod.json", "{\"costumes\": [{\"id\": \"original\", \"name\": \"Hero D\", \"entity\": \"" + dEntity +
                 "\", \"animation_banks\": {\"SUPERMAN_MELEEAIR\": \"banks/too_big.bin\"}}]}");
        writeBin(mods / "hero_d/banks/too_big.bin", std::string(flight.begin(), flight.end()));
        writeBin(mods / "hero_d/DATA/_workinprogress/hero_d" / (dEntity + ".xml"), std::string("<entity/>\0", 10));
        const auto registryRoot = [](const fs::path& table) { return table.parent_path().parent_path().parent_path().parent_path(); };
        auto banksEnv = envFor(root, "hero_a;hero_c", "0", true);
        banksEnv.startupCostume = "hero_c/original";
        K::modOverlayResetForTestEnv(banksEnv);
        std::string family;
        int cSlot = 0, aSlot = 0;  // the list reads by display name: assert identities, not positions
        for (const auto& c : K::modCostumeList()) {
          if (c.id == "hero_c/original") { family = c.bankFamily; cSlot = c.slot; }
          if (c.id == "hero_a/original") aSlot = c.slot;
        }
        const fs::path banksTable = tablePath(K::modActiveLayers());
        const std::string banksTableText = readBin(banksTable);
        const std::string servedText = banksTable.empty() ? std::string() : readBin(registryRoot(banksTable) / "DATA/baseworl.AST");
        const std::vector<uint8_t> served(servedText.begin(), servedText.end());
        std::vector<uint8_t> servedAnim, servedSlot;
        const bool servedOk = K::modReadNestedEntry(served, "BASEWORLDFLOW_ANIM", &servedAnim, &err) &&
                              K::modReadNestedEntry(servedAnim, "SUPERMAN_MELEEAIR", &servedSlot, &err);
        check(family.size() == 22 && family.rfind("banks_", 0) == 0 && cSlot >= 5 && cSlot <= 6 && aSlot >= 5 && aSlot <= 6 && aSlot != cSlot && servedOk && servedSlot == superspeed &&
                  K::modCostumeSessionFamily() == family &&
                  banksTableText.find("key=\"SRActiveFamily\" type=\"String\" category=\"0\" constant=\"1\" value=\"" + family + "\"") != std::string::npos &&
                  banksTableText.find("key=\"SRCostumeFamily" + std::to_string(cSlot) + "\" type=\"String\" category=\"0\" constant=\"1\" value=\"" + family + "\"") != std::string::npos &&
                  banksTableText.find("key=\"SRCostumeFamily" + std::to_string(aSlot) + "\" type=\"String\" category=\"0\" constant=\"1\" value=\"retail\"") != std::string::npos &&
                  banksTableText.find("key=\"SRWornCostume\" type=\"Integer\" category=\"0\" constant=\"0\" value=\"" + std::to_string(cSlot) + "\"") != std::string::npos,
              "AB5 worn at boot, an own-bank suit's banks are served in baseworl.AST and its family is the session's");

        // the bridge: in this session a retail suit is worn from the next start
        const std::wstring contentDir = (root / "content").wstring();
        const wchar_t* priorContent = _wgetenv(L"SR_CONTENT_DIR");
        const std::wstring savedContent = priorContent ? priorContent : L"";
        _wputenv_s(L"SR_CONTENT_DIR", contentDir.c_str());
        std::vector<K::ModReply> wornReplies;
        const bool wornHandled = K::modBridgeDispatchEx("SR_CostumeWorn", "1", &wornReplies) == K::ModBridgeResult::ReplyInt;
        check(wornHandled && wornReplies.size() == 1 && wornReplies[0].method == "SR_CostumeResult" &&
                  wornReplies[0].args.size() == 3 && wornReplies[0].args[0].s == "retail/2" && wornReplies[0].args[1].i == 0 &&
                  wornReplies[0].args[2].s.find("next start") != std::string::npos &&
                  readBin(root / "content/settings/costume.ini").find("worn=retail/2") != std::string::npos,
              "AB6 a retail suit in an own-bank session is remembered and worn from the next start (no retail switch now)");

        auto retailEnv = banksEnv;
        retailEnv.startupCostume = "hero_a/original";
        K::modOverlayResetForTestEnv(retailEnv);
        const fs::path retailTable = tablePath(K::modActiveLayers());
        const std::string retailText = readBin(retailTable);
        std::error_code bec;
        wornReplies.clear();
        check(!retailTable.empty() && !fs::exists(registryRoot(retailTable) / "DATA/baseworl.AST", bec) &&
                  K::modCostumeSessionFamily() == "retail" &&
                  retailText.find("key=\"SRActiveFamily\" type=\"String\" category=\"0\" constant=\"1\" value=\"retail\"") != std::string::npos &&
                  retailText.find("key=\"SRCostumeFamily" + std::to_string(cSlot) + "\" type=\"String\" category=\"0\" constant=\"1\" value=\"" + family + "\"") != std::string::npos &&
                  K::modBridgeDispatchEx("SR_CostumeWorn", "2", &wornReplies) == K::ModBridgeResult::ReplyInt &&
                  wornReplies.size() == 1 && wornReplies[0].args[1].i == 1,
              "AB7 a retail-family session serves the disc's banks; retail suits switch at once; the own-bank suit keeps its family");

        auto bigEnv = envFor(root, "hero_a;hero_d", "0", true);
        bigEnv.startupCostume = "hero_d/original";
        K::modOverlayResetForTestEnv(bigEnv);
        const fs::path bigTable = tablePath(K::modActiveLayers());
        const std::string bigText = readBin(bigTable);
        check(!bigTable.empty() && !fs::exists(registryRoot(bigTable) / "DATA/baseworl.AST", bec) &&
                  bigText.find("key=\"SRWornCostume\" type=\"Integer\" category=\"0\" constant=\"0\" value=\"0\"") != std::string::npos &&
                  bigText.find("key=\"SRActiveFamily\" type=\"String\" category=\"0\" constant=\"1\" value=\"retail\"") != std::string::npos,
              "AB8 banks that do not fit fall back to the retail banks AND the retail default suit (never a stretched body)");

        for (const char* manifest : {
                 "\"animation_banks\": {\"SUPERMAN_CAPE\": \"banks/meleeair.bin\"}",
                 "\"animation_banks\": {\"SUPERMAN_MELEEAIR\": \"DATA/meleeair.bin\"}",
                 "\"animation_banks\": {\"SUPERMAN_MELEEAIR\": \"mod.json\"}",
                 "\"animation_banks\": {}",
                 "\"animation_project\": \"SRM_X\", \"animation_banks\": {\"SUPERMAN_MELEEAIR\": \"banks/meleeair.bin\"}"}) {
          writeBin(mods / "hero_e/mod.json", "{\"costumes\": [{\"id\": \"original\", \"name\": \"Hero E\", \"entity\": \"" +
                   K::modAssetPrefix("hero_e") + "ORIGINAL_ENT\", " + manifest + "}]}");
          writeBin(mods / "hero_e/banks/meleeair.bin", std::string(superspeed.begin(), superspeed.end()));
          writeBin(mods / "hero_e/DATA/meleeair.bin", std::string(superspeed.begin(), superspeed.end()));
          K::modOverlayResetForTestEnv(envFor(root, "hero_e", "0", true));
          const auto* e = findMod(K::modMenuList(), "hero_e");
          check(e && e->status == K::ModStatus::BadManifest, (std::string("AB9 refused animation_banks manifest: ") + manifest).c_str());
        }
        _wputenv_s(L"SR_CONTENT_DIR", savedContent.c_str());
        writeBin(fixtureBase, fixtureBefore);
        fs::remove_all(mods / "hero_c", bec);
        fs::remove_all(mods / "hero_d", bec);
        fs::remove_all(mods / "hero_e", bec);
      }

      const std::string altEntity = K::modAssetPrefix("hero_b") + "ALTERNATE_ENT";
      const std::string originalEntity = K::modAssetPrefix("hero_b") + "ORIGINAL_ENT";
      writeBin(mods / "hero_b" / "mod.json", "{\"costumes\":[{\"id\":\"original\",\"name\":\"Original\",\"entity\":\"" +
               originalEntity + "\"},{\"id\":\"alternate\",\"name\":\"Alternate\",\"entity\":\"" + altEntity + "\"}]}");
      writeBin(mods / "hero_b/DATA/_workinprogress/hero_b" / (altEntity + ".xml"), "<entity/>");
      writeBin(mods / "mods.json", "{\"order\":[\"hero_b\",\"hero_a\"],\"disabled\":[],\"disabled_costumes\":[\"missing/later\",\"../bad\",5]}");
      K::modOverlayResetForTestEnv(player);
      const std::string includedBoot = readBin(tablePath(K::modActiveLayers()));
      check(K::modCostumeList().size() == 3 && K::modCostumeRegistryReady(),
            "S1 a multi-suit pack and another character pack are all available together by default");
      check(K::modCostumeSetIncluded("HERO_B/ORIGINAL", false) && K::modMenuRestartRequired() &&
                K::modCostumeList().size() == 3 && readBin(tablePath(K::modActiveLayers())) == includedBoot,
            "S2 checkbox saves stable identity without mutating the running content registry");
      auto choices = K::modCostumePackChoices("HERO_B");
      check(choices.size() == 2 && !choices[0].included && choices[0].active && choices[1].included && choices[1].active,
            "S3 pack details distinguish saved inclusion from currently mounted suits");
      std::vector<K::ModReply> suitReplies;
      check(K::modBridgeDispatchEx("SR_ModSuitList", "hero_b", &suitReplies) == K::ModBridgeResult::ReplyInt &&
                suitReplies.size() == 6 && suitReplies[1].method == "SR_ModSuit" && suitReplies[1].args[2].i == 2 &&
                suitReplies[3].args[1].s == "hero_b/alternate" && suitReplies[3].args[2].i == 3,
            "S14 real AS bridge delivers independent inclusion and current-boot flags for both siblings");
      K::modOverlayResetForTestEnv(player);
      const auto includedSuits = K::modCostumeList();
      const std::string filteredTable = readBin(tablePath(K::modActiveLayers()));
      // the list reads by display name: the tests below assert stable identities and the slot the
      // list gave each, never a particular order
      int altSlot = -1, heroASlot = -1;
      for (const auto& c : includedSuits) {
        if (c.id == "hero_b/alternate") altSlot = c.slot;
        if (c.id == "hero_a/original") heroASlot = c.slot;
      }
      check(includedSuits.size() == 2 && altSlot >= 5 && altSlot <= 6 && heroASlot >= 5 && heroASlot <= 6 &&
                altSlot != heroASlot &&
                filteredTable.find(altEntity) != std::string::npos && filteredTable.find(originalEntity) == std::string::npos &&
                filteredTable.find("SupermanCrazyEnt") != std::string::npos && !K::modMenuRestartRequired(),
            "S4 next boot excludes only the unchecked suit and preserves retail and independent additions");
      check(includedSuits.size() == 2 && includedSuits[0].name == "Alternate" && includedSuits[0].slot == 5 &&
                includedSuits[1].id == "hero_a/original" && includedSuits[1].slot == 6,
            "S4b the list reads by display name, case-insensitive (Alternate before hero_a), not by stable id");
#ifdef _WIN32
      const char* nativeGate = std::getenv("SR_NATIVE_COSTUMES");
      const std::string savedNativeGate = nativeGate ? nativeGate : "";
      _putenv_s("SR_NATIVE_COSTUMES", "0");
      std::vector<K::ModReply> catalog;
      K::modBridgeDispatchEx("SR_ModCostumes", "", &catalog);
      check(catalog.size() == 2 && catalog[0].method == "SR_CostumeBegin" && catalog[0].args[0].i == 0,
            "T1 native picker gate off returns no additions, leaving all retail records alone");
      _putenv_s("SR_NATIVE_COSTUMES", "1");
      catalog.clear();
      K::modBridgeDispatchEx("SR_ModCostumes", "", &catalog);
      const std::set<std::string> listed = catalog.size() == 10 ? std::set<std::string>{catalog[1].args[1].s, catalog[5].args[1].s}
                                                                 : std::set<std::string>{};
      check(catalog.size() == 10 && catalog[0].args[0].i == 2 &&
                listed == std::set<std::string>{"hero_a/original", "hero_b/alternate"} && catalog.back().method == "SR_CostumeEnd",
            "T2 native picker lists both active packs and omits the unchecked sibling");
      check(catalog.back().args.size() == 2 && catalog.back().args[1].i == 24,
            "T2b short suit names keep the retail name font (24) on the one-line name field");
      K::ModCostumeInfo found;
      check(K::modCostumeFindActive("HERO_B/ALTERNATE", &found) && found.slot == altSlot &&
                !K::modCostumeFindActive("hero_b/original", &found) && !K::modCostumeFindActive("../bad", &found),
            "T3 selection resolves an active stable identity, not a retail slot or excluded suit");

      const std::string stateBeforeWear = readBin(mods / "mods.json");
      PPCContext wearContext{};
      wearContext.r1.u32 = 0x90820000u;
      wearContext.r3.u64 = 0xABCDEFu;
      const PPCContext beforeWear = wearContext;
      std::vector<PPCFunc*> requestTable(K::kDispatchTableEntryCount, nullptr);
      PPCFunc** oldTable = K::g_dispatchTable;
      K::g_dispatchTable = requestTable.data();
      requestTable[(0x821F6850u - PPC_CODE_BASE) >> 2] = &costumeFind;
      requestTable[(0x821E6F90u - PPC_CODE_BASE) >> 2] = &costumeSet;
      uint8_t* base = K::getGuestBase();
      const uint32_t oldManagerGlobal = PPC_LOAD_U32(0x8296DF74u);
      K::writeGuestBE32(0x8296DF74u, kCostumeMgr);
      std::memset(K::guestVAToHost(kCostumeMgr), 0, 0x500);
      K::writeGuestBE32(kCostumeMgr + 0x20, kCostumeFlows);
      K::writeGuestBE32(kCostumeFlows, kCostumeFlow);
      K::writeGuestBE32(kCostumeFlow + 0x4C, 0x7B1996E6u);
      K::writeGuestBE32(kCostumeFlow + 104, kCostumeDas);
      K::writeGuestBE32(kCostumeDas + 4, 0x90810400u);
      K::writeGuestBE32(0x90810400u, 0x90810500u);
      K::writeGuestBE32(0x90810404u, 0x90810504u);
      K::writeGuestBE32(0x90810500u, 0x90810600u);
      K::writeGuestBE32(0x90810600u, 0x2E196D85u);
      K::writeGuestBE32(0x90810604u, 0);
      K::writeGuestBE32(0x90810608u, 0);
      const auto wear = [&](const std::string& id, PPCContext* context) {
        std::vector<K::ModReply> result;
        K::modBridgeDispatchEx("SR_CostumeWear", id, &result, context);
        return result.size() == 1 && result[0].method == "SR_CostumeResult" && result[0].args[1].i == 1;
      };
      g_costumeFinds = g_costumeSets = g_requestedSlot = 0;
      g_findCostumeFlow = kCostumeFlow;
      g_setCostumeResult = 0x90810400u;
      g_costumeArguments = true;
      check(wear("hero_b/alternate", &wearContext) && g_costumeFinds == 1 && g_costumeSets == 1 &&
                g_requestedSlot == altSlot && g_costumeArguments &&
                std::memcmp(&wearContext, &beforeWear, sizeof wearContext) == 0,
            "T4 wear requests the retail flow and setter on the caller context with the correct appended slot");
      check(!wear("hero_b/original", &wearContext) && !wear("6", &wearContext) &&
                !wear("hero_b/alternate extra", &wearContext) && !wear("hero_b/alternate", nullptr) &&
                g_costumeSets == 1 && readBin(mods / "mods.json") == stateBeforeWear,
            "T5 excluded, numeric, malformed and context-free wear requests do nothing; no restart-selection save");
      g_findCostumeFlow = 0;
      check(!wear("hero_b/alternate", &wearContext) && g_costumeSets == 1 &&
                std::memcmp(&wearContext, &beforeWear, sizeof wearContext) == 0,
            "T6 absent actor flow refuses wear and restores interpreter registers");
      g_findCostumeFlow = kCostumeFlow;
      g_setCostumeResult = 0;
      check(!wear("hero_b/alternate", &wearContext) && g_costumeSets == 2,
            "T7 a retail setter that found no DAS node is not reported as a queued request");
      g_setCostumeResult = 0x90810400u;
      std::memcpy(K::guestVAToHost(0x90821000u), "SR_CostumeWear", sizeof("SR_CostumeWear"));
      std::memcpy(K::guestVAToHost(0x90821100u), "hero_b/alternate", sizeof("hero_b/alternate"));
      wearContext.r3.u32 = 0x90821000u;
      wearContext.r4.u32 = 0x90821100u;
      sub_826D39A0(wearContext, K::getGuestBase());
      check(g_costumeSets == 3 && g_requestedSlot == altSlot && wearContext.r1.u32 == beforeWear.r1.u32 &&
                readBin(mods / "mods.json") == stateBeforeWear,
            "T8 real FSCommand handler passes its interpreter context to the wear request");
      bool malformedRefused = true;
      for (const auto& mutation : {std::make_pair(kCostumeDas + 4, 0xFFFFFFFFu),
                                   std::make_pair(0x90810404u, 0x908104FCu),
                                   std::make_pair(0x90810404u, 0x90810503u),
                                   std::make_pair(0x90810500u, 0xFFFFFFFFu),
                                   std::make_pair(0x90810608u, 0x20000u),
                                   std::make_pair(0x90810608u, 0x10000u)}) {
        const uint32_t original = PPC_LOAD_U32(mutation.first);
        K::writeGuestBE32(mutation.first, mutation.second);
        malformedRefused = malformedRefused && !wear("hero_b/alternate", &wearContext);
        K::writeGuestBE32(mutation.first, original);
      }
      check(malformedRefused && g_costumeSets == 3,
            "T12 unreadable root, reversed/unaligned vector, invalid node, wrong type and constant node never reach the setter");
      DWORD oldProtect = 0, restoredProtect = 0;
      // The movie fix enables HRR caching. Fixture page mutations must use the
      // production removal protocol, including permission restoration.
      auto protectFixture = [](uint32_t address, DWORD protection, DWORD* old) {
        K::hostRangeReadableBeginRemoval();
        const bool changed = VirtualProtect(K::guestVAToHost(address), 0x1000, protection, old) != 0;
        K::hostRangeReadableEndRemoval();
        return changed;
      };
      const bool madeReadonly = protectFixture(0x90810000u, PAGE_READONLY, &oldProtect);
      const bool readonlyRefused = madeReadonly && !wear("hero_b/alternate", &wearContext) && g_costumeSets == 3;
      const bool restoredReadonly = madeReadonly &&
        protectFixture(0x90810000u, oldProtect, &restoredProtect);
      check(readonlyRefused && restoredReadonly, "T14 a readable but read-only DAS target is refused without entering the setter");
      if (!restoredReadonly) std::abort(); // never continue fixture writes into a protected page
      K::writeGuestBE32(kCostumeDas + 4, 0x90830000u);
      const bool madeUnreadable = protectFixture(0x90830000u, PAGE_NOACCESS, &oldProtect);
      const bool unreadableRefused = madeUnreadable && !wear("hero_b/alternate", &wearContext) && g_costumeSets == 3;
      const bool restoredUnreadable = madeUnreadable &&
        protectFixture(0x90830000u, oldProtect, &restoredProtect);
      check(unreadableRefused && restoredUnreadable, "T15 a nonzero aligned root on a no-access page is refused fault-free");
      if (!restoredUnreadable) std::abort();
      K::writeGuestBE32(kCostumeDas + 4, 0x90810400u);
      requestTable[(0x821F6850u - PPC_CODE_BASE) >> 2] = &sub_821F6850;
      requestTable[(0x821E6F90u - PPC_CODE_BASE) >> 2] = &sub_821E6F90;
      check(wear("hero_b/alternate", &wearContext) && PPC_LOAD_U32(0x90810604u) == static_cast<uint32_t>(altSlot) &&
                readBin(mods / "mods.json") == stateBeforeWear,
            "T13 real recompiled flow lookup and DAS setter resolve SetCostumeNum and write the appended slot (no controlled leaves)");
      requestTable[(0x821F6850u - PPC_CODE_BASE) >> 2] = &costumeFind;
      requestTable[(0x821E6F90u - PPC_CODE_BASE) >> 2] = &costumeSet;
      _putenv_s("SR_NATIVE_COSTUMES", "0");
      check(!wear("hero_b/alternate", &wearContext) && g_costumeSets == 3,
            "T9 disabled native-picker rollout gate blocks wear as well as listing");
      K::writeGuestBE32(0x8296DF74u, oldManagerGlobal);
      K::g_dispatchTable = oldTable;
      _putenv_s("SR_NATIVE_COSTUMES", savedNativeGate.c_str());
#endif
      check(K::modCostumeSetIncluded("hero_b/alternate", false) && K::modCostumeSetIncluded("hero_a/original", false) &&
                readBin(mods / "mods.json").find("missing/later") != std::string::npos &&
                readBin(mods / "mods.json").find("../bad") == std::string::npos,
            "S5 multiple checkboxes coexist and absent valid suit choices survive a save");
      K::modOverlayResetForTestEnv(player);
      check(K::modCostumeList().empty() && !K::modCostumeRegistryReady() && tablePath(K::modActiveLayers()).empty(),
            "S6 no included mod suits means no shared table override; retail assets remain untouched");
      check(K::modMenuSetEnabled("hero_b", false), "S7 a pack master switch remains independent of its suit checkboxes");
      K::modOverlayResetForTestEnv(player);
      choices = K::modCostumePackChoices("hero_b");
      check(choices.size() == 2 && !choices[0].included && !choices[1].included && !choices[0].active &&
                K::modCostumeSetIncluded("hero_b/alternate", true),
            "S8 suits in a disabled pack can be prepared without activating the pack");
      check(K::modMenuSetEnabled("hero_b", true), "S9 enabling a prepared pack retains all per-suit choices");
      K::modOverlayResetForTestEnv(player);
      check(K::modCostumeList().size() == 1 && K::modCostumeList()[0].id == "hero_b/alternate" &&
                K::modCostumeList()[0].slot == 5, "S10 only the included sibling returns, regardless of reassigned numeric slot");
      const std::string inclusionState = readBin(mods / "mods.json");
      check(!K::modCostumeSetIncluded("missing/later", true) && !K::modCostumeSetIncluded("hero_b/../alternate", true) &&
                readBin(mods / "mods.json") == inclusionState, "S11 unknown and malformed suit toggles cannot change state");
      fs::create_directory(mods / "mods.json.tmp", ec);
      check(!K::modCostumeSetIncluded("hero_a/original", true) && !K::modCostumePackChoices("hero_a")[0].included &&
                !K::modMenuRestartRequired() && readBin(mods / "mods.json") == inclusionState,
            "S12 failed checkbox save rolls back memory, disk and restart status");
      fs::remove(mods / "mods.json.tmp", ec);
      K::modOverlayResetForTestEnv(envFor(root, "@player", "0", true));
      check(!K::modCostumeSetIncluded("hero_b/original", true) && readBin(mods / "mods.json") == inclusionState,
            "S13 automated launches cannot modify player suit inclusion");
      suitReplies.clear();
      check(K::modBridgeDispatchEx("SR_ModSuitSet", "hero_b/original 1", &suitReplies) == K::ModBridgeResult::ReplyInt &&
                !suitReplies.empty() && suitReplies[0].method == "SR_ModSuitSaved" && suitReplies[0].args[1].i == 0 &&
                readBin(mods / "mods.json") == inclusionState,
            "S15 AS bridge reports refused checkbox changes without writing automated player state");
      K::modMenuResetForTest();
#ifdef _WIN32
      _putenv_s("SR_MOD_MANAGER", hadManager ? savedManager.c_str() : "");
#endif
    }
  }

  // Browser metadata is ordinary validated manifest data, not a skin-selection
  // request. It must remain usable for legacy folders and mixed-category mods.
  {
    const fs::path browser = root / "browser_metadata";
    writeBin(browser / "Superman Returns (USA)/DATA/marker", "data");
    writeBin(browser / "mods/mission/mod.json", "{\"category\":\"MISSIONS\",\"preview\":\"preview.png\"}");
    writeBin(browser / "mods/mission/preview.png", "PNG");
    writeBin(browser / "mods/legacy/DATA/legacy.txt", "data");
    writeBin(browser / "mods/badtype/mod.json", "{\"category\":5}");
    writeBin(browser / "mods/badcategory/mod.json", "{\"category\":\"not-a-filter\"}");
    for (const std::string id : {"preview_ok", "preview_url", "preview_retail", "preview_long", "preview_type"}) {
      const auto prefix = K::modAssetPrefix(id);
      const std::string thumbnail = id == "preview_url" ? "https://example.com/p.png" :
        id == "preview_retail" ? "COSTUMETHUMB_1" : id == "preview_long" ? prefix + "TOOLONGNAME" : prefix + "TH1";
      writeBin(browser / "mods" / id / "mod.json", "{\"costumes\":[{\"id\":\"one\",\"name\":\"Preview test\","
        "\"entity\":\"" + prefix + "ENT\",\"thumbnail\":\"" + thumbnail + "\",\"fullsize\":\"" + prefix +
        "FL1\",\"description\":" + (id == "preview_type" ? "5" : "\"Own suit preview\"") + "}]}");
    }
    K::modOverlayResetForTestEnv(envFor(browser, nullptr, "0", false));
    const auto list = K::modMenuList();
    const auto* mission = findMod(list, "mission");
    const auto* legacy = findMod(list, "legacy");
    const auto* badtype = findMod(list, "badtype");
    const auto* badcategory = findMod(list, "badcategory");
    check(mission && mission->status == K::ModStatus::Ok && mission->category == "missions" && !mission->preview.empty(),
          "Q1 browser category normalized; existing local preview retained");
    check(legacy && legacy->category == "other" && K::modCategoryLabel("characters") == "Skins & Characters",
          "Q2 legacy category fallback and readable character filter label");
    check(badtype && badcategory && badtype->status == K::ModStatus::BadManifest && badcategory->status == K::ModStatus::BadManifest,
          "Q3 non-string and unsupported categories refused without mounting");
    const auto* previews = findMod(list, "preview_ok");
    check(previews && previews->status == K::ModStatus::Ok && previews->costumes.size() == 1 &&
              previews->costumes[0].thumbnail == K::modAssetPrefix("preview_ok") + "TH1" &&
              previews->costumes[0].description == "Own suit preview",
          "T10 scoped native thumbnail/fullsize metadata and per-suit description survive discovery");
    bool badPreviews = true;
    for (const char* id : {"preview_url", "preview_retail", "preview_long", "preview_type"}) {
      const auto* m = findMod(list, id);
      badPreviews = badPreviews && m && m->status == K::ModStatus::BadManifest;
    }
    check(badPreviews, "T11 costume previews refuse URLs, retail collisions, overlength movie names and wrong value types");
    std::vector<K::ModMenuCall> calls;
    const bool listed = K::modMenuCommand("SR_ModList", "", &calls);
    check(listed && std::any_of(calls.begin(), calls.end(), [](const auto& call) {
      return call.method == "SR_ModText" && call.args.size() == 3 && call.args[1].s == "category" && call.args[2].s == "missions";
    }), "Q4 browser category is delivered through the real AS metadata bridge");
  }
  // ---- D: population / traffic density (iter-466, kernel/mod_density.hpp) ------------
  {
    const std::vector<uint8_t> doc = trafficDoc();
    std::vector<uint8_t> plain, text;
    std::string err, sum;
    check(K::modCompiledXmlToText(doc, &plain, &err), "D0 the synthetic trafficDef converts with the generic converter");
    const std::string p(plain.begin(), plain.end());
    K::ModDensity d;
    d.traffic = 200;
    d.population = 50;
    const bool ok = K::modComposeTrafficDensity(doc, d, &text, &sum, &err);
    std::string want = p;
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"type=\"Car\" count=\"60\"", "type=\"Car\" count=\"120\""},
             {"type=\"ParkedCar\" count=\"40\"", "type=\"ParkedCar\" count=\"80\""},
             {"type=\"Pedestrian\" radiusScale=\"0.8\" count=\"60\"", "type=\"Pedestrian\" radiusScale=\"0.8\" count=\"30\""}}) {
      const size_t at = want.find(from);
      if (at != std::string::npos) want.replace(at, from.size(), to);
    }
    check(ok && std::string(text.begin(), text.end()) == want && want != p && !text.empty() && text.back() == 0 &&
              sum == "Car 60->120, ParkedCar 40->80, Pedestrian 60->30",
          "D1 200%/50%: only the Car/ParkedCar (traffic) and Pedestrian (population) counts change; MEFireTruck stays 0");
    check(K::modComposeTrafficDensity(doc, K::ModDensity{}, &text, &sum, &err) && std::string(text.begin(), text.end()) == p,
          "D2 100%/100% composes exactly the generic text document");
    d.traffic = 110;
    check(!K::modComposeTrafficDensity(doc, d, &text, &sum, &err) && !K::modDensityValid(110) && !K::modDensityValid(0) &&
              !K::modDensityValid(225) && K::modDensityValid(25) && K::modDensityValid(200),
          "D3 only 25..200 in steps of 25 are accepted");
    check(!K::modComposeTrafficDensity(trafficDoc(false), K::ModDensity{}, &text, &sum, &err) &&
              err.find("Pedestrian") != std::string::npos,
          "D4 a document without the Pedestrian set fails closed");
    check(K::modDensityScale(60, 25) == 15 && K::modDensityScale(40, 75) == 30 && K::modDensityScale(60, 175) == 105 &&
              K::modDensityScale(7, 50) == 4,
          "D5 counts scale by percent, rounding half up");

    // boot: a separate content root whose disc holds only metropol.AST
    const fs::path droot = fs::temp_directory_path(ec) / ("sr_density_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(droot, ec);
    const std::string compiled(doc.begin(), doc.end());
    writeBin(droot / "Superman Returns (USA)" / "DATA" / "metropol.AST",
             bgfa({{"METROPOLISWORLDFLOW_SCRIPT", bgfa({{"X", "x"}})},
                   {"METROPOLISWORLDFLOW_OTHER", bgfa({{"OTHERDOC", "o"}, {"METROPOLISTRAFFIC", compiled}})}}));
    auto densityDoc = [&]() -> std::string {
      for (const auto& L : K::modActiveLayers())
        if (L.id == "density" && L.port && !L.roots.empty())
          return readBin(fs::u8path(L.roots[0]) / "DATA" / "_workinprogress" / "_density" / "METROPOLISTRAFFIC.xml");
      return "";
    };
    K::ModBootEnv e = envFor(droot, nullptr, nullptr, false);  // a player launch: port features on
    e.allowStateWrite = false;
    e.trafficDensity = 150;
    e.populationDensity = 200;
    K::modManagerResetForTest(e);
    const std::string served = densityDoc();
    check(served.find("type=\"Car\" count=\"90\"") != std::string::npos &&
              served.find("type=\"ParkedCar\" count=\"60\"") != std::string::npos &&
              served.find("count=\"120\"") != std::string::npos && K::modDensityApplied().traffic == 150 &&
              K::modDensityApplied().population == 200 && K::modBootSummary().find("density") != std::string::npos,
          "D6 player launch at 150/200: a port layer serves DATA/_workinprogress/_density/METROPOLISTRAFFIC.xml, scaled");
    e.trafficDensity = 100;
    e.populationDensity = 100;
    K::modManagerResetForTest(e);
    check(densityDoc().empty() && K::modDensityApplied() == K::ModDensity{} &&
              K::modBootSummary().find("density") == std::string::npos,
          "D7 100/100 mounts nothing (the retail document, byte-identical)");
    K::ModBootEnv a = envFor(droot, nullptr, nullptr, true);  // an automated (natural) roll
    a.trafficDensity = 200;
    K::modManagerResetForTest(a);
    const bool naturalNone = densityDoc().empty();
    a = envFor(droot, nullptr, "1", true);
    a.trafficDensity = 200;
    K::modManagerResetForTest(a);
    check(naturalNone && densityDoc().find("type=\"Car\" count=\"120\"") != std::string::npos,
          "D8 a natural roll serves no density layer; SR_PORT_FEATURES=1 turns it on");
    a = envFor(droot, "custom_frontend", nullptr, true);
    a.trafficDensity = 200;
    K::modManagerResetForTest(a);
    const bool listedOther = densityDoc().empty();
    a = envFor(droot, "density", nullptr, true);
    a.trafficDensity = 200;
    K::modManagerResetForTest(a);
    check(listedOther && !densityDoc().empty(), "D9 an explicit SR_MODS list applies density only when it names it");
    a.trafficDensity = 120;
    K::modManagerResetForTest(a);
    check(densityDoc().empty() && K::modDensityApplied() == K::ModDensity{}, "D10 an invalid percent boots the retail document");

    // the store: game_options.ini keeps the keys when the game options screen saves
#ifdef _WIN32
    const wchar_t* priorContent = _wgetenv(L"SR_CONTENT_DIR");
    const std::wstring savedContent = priorContent ? priorContent : L"";
    _wputenv_s(L"SR_CONTENT_DIR", (droot / "content").wstring().c_str());
    writeBin(droot / "content" / "settings" / "game_options.ini",
             "# note\nmusic_volume=7\nsfx_volume=8\ncontrol_scheme=1\ninvert_x=0\ninvert_y=1\nauto_center=1\nvibration=1\n"
             "apply_to_loaded_saves=1\nother_pc_option=3\n");
    const bool none = K::modDensityLoad() == K::ModDensity{};
    K::ModDensity s;
    s.traffic = 200;
    s.population = 75;
    const bool saved = K::modDensitySave(s) && K::modDensityLoad() == s;
    const std::string afterSave = readBin(droot / "content" / "settings" / "game_options.ini");
    int32_t canon = 0;
    const int32_t packed = K::modSettingsLoadPacked();
    const bool game = packed >= 0 && K::modSettingsSavePacked(packed & 0x3FFF, true, &canon);
    const std::string afterGame = readBin(droot / "content" / "settings" / "game_options.ini");
    check(none && saved && afterSave.find("music_volume=7") != std::string::npos &&
              afterSave.find("other_pc_option=3") != std::string::npos && game && K::modDensityLoad() == s &&
              afterGame.find("other_pc_option=3") != std::string::npos && K::modSettingsLoadPacked() == packed,
          "D11 game_options.ini: density keys round-trip, keep every other line, and survive the game options save");
    writeBin(droot / "content" / "settings" / "game_options.ini", "traffic_density=110\npopulation_density=abc\n");
    check(K::modDensityLoad() == K::ModDensity{}, "D12 invalid stored values read as 100");
    _wputenv_s(L"SR_CONTENT_DIR", savedContent.c_str());
#endif

    // the real disc's document (skipped without the disc)
    const std::string realDisc = K::contentRootDir() + "Superman Returns (USA)";
    std::vector<uint8_t> top, real, out;
    if (K::modReadTopEntry(realDisc + "/DATA/metropol.AST", "METROPOLISWORLDFLOW_OTHER", &top, &err) &&
        K::modReadNestedEntry(top, "METROPOLISTRAFFIC", &real, &err)) {
      K::ModDensity r;
      r.traffic = 200;
      r.population = 200;
      check(real.size() == 3996 && K::modComposeTrafficDensity(real, r, &out, &sum, &err) &&
                sum == "Car 60->120, ParkedCar 40->80, Pedestrian 60->120",
            "D13 the retail METROPOLISTRAFFIC: Car 60, ParkedCar 40, Pedestrian 60 -> 120 / 80 / 120 at 200%");
      std::printf("       real METROPOLISTRAFFIC at 200%%: %s\n", sum.c_str());
    } else {
      std::printf("  --  D13 skipped: no retail metropol.AST (%s)\n", err.c_str());
    }
    fs::remove_all(droot, ec);
  }

  K::modOverlayResetForTest(root.generic_string(), {});
  fs::remove_all(root, ec);
  std::printf("[MOD_MANAGER_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
