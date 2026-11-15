// step5 E14a (lane B): the achievements core (kernel/achievements.hpp) - the XDBF table, the sidecar, the save-file
// checksum, the XMsgStartIORequest hook through the REAL export registry, and the save events.
//
//   build/difftest.exe --achievements-test                  exit = failed checks (expect 0)
//   SR_ACH_XDBF=<the title's XDBF, extracted> ...            adds R1 (the real table: 18 achievements, 1000 G,
//                                                            title 454107ED, 7 secret); skipped (not counted) unset
//   SR_ACHIEVEMENTS=0 ... --achievements-test                the gate's MUTANT: H1-H4, E2-E11 FAIL (nothing hooked)
// Guest scratch at 0x90780000 (the import-fixes test uses 0x90700000..); every file test runs in its own temp folder.
// The review's fixes (2026-10-24), failing-first on a515f014: H9 (an uncommitted / NOACCESS guest page is not a
// readable span; the hook refuses it without a fault), S15 (non-canonical / aliased sidecar ids), W1-W3 (SR_SaveEvent
// through the real mod bridge: the achievements act on the slot the EVENT names, not the tracker's remembered one).
// E5 and E14 were tightened (a union would now fail E5; E14 compares the save file's bytes, not its size).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/achievements.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/mod_bridge.hpp"
#include "kernel/stubs.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // H9: VirtualProtect makes one guest page unreadable

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace K = sr::kernel;
namespace A = sr::ach;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

// ------------------------------------------------------------------------------------------- fixtures
void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xFFFF); }
void putTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

struct XdbfBuilder {
  struct E { uint16_t sec; uint64_t id; std::vector<uint8_t> data; };
  std::vector<E> es;
  void add(uint16_t sec, uint64_t id, std::vector<uint8_t> d) { es.push_back({sec, id, std::move(d)}); }
  std::vector<uint8_t> build() const {
    const uint32_t capacity = uint32_t(es.size()) + 2, freeCap = 1;
    std::vector<uint8_t> v;
    putTag(v, "XDBF");
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
std::vector<uint8_t> section(const char* tag, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> v;
  putTag(v, tag);
  put32(v, 1);
  put32(v, uint32_t(12 + body.size()));
  v.insert(v.end(), body.begin(), body.end());
  return v;
}
struct Row { uint16_t id, name, desc, howto; uint32_t image; uint16_t score; uint32_t flags; };
std::vector<uint8_t> xach(const std::vector<Row>& rows) {
  std::vector<uint8_t> b;
  put16(b, uint32_t(rows.size()));
  for (const Row& r : rows) {
    put16(b, r.id); put16(b, r.name); put16(b, r.desc); put16(b, r.howto);
    put32(b, r.image); put16(b, r.score); put16(b, 0); put32(b, r.flags);
    for (int k = 0; k < 4; k++) put32(b, 0);
  }
  return section("XACH", b);
}
std::vector<uint8_t> xstr(const std::vector<std::pair<uint16_t, std::string>>& s) {
  std::vector<uint8_t> b;
  put16(b, uint32_t(s.size()));
  for (const auto& [id, t] : s) {
    put16(b, id);
    put16(b, uint32_t(t.size()));
    b.insert(b.end(), t.begin(), t.end());
  }
  return section("XSTR", b);
}
constexpr uint32_t kTitle = 0x454107EDu;
constexpr uint64_t kIdXach = 0x58414348u, kIdXstc = 0x58535443u, kIdXthd = 0x58544844u;
// Two achievements: id 11 (shown while locked, 50 G) and id 12 (secret, 0 G); English (1) and German (3).
std::vector<uint8_t> fixtureTable(bool withXstc = true) {
  XdbfBuilder x;
  x.add(1, kIdXach, xach({{11, 1, 2, 3, 0x10B, 50, 0x8 | 1}, {12, 4, 5, 6, 0x10C, 0, 2}}));
  if (withXstc) { std::vector<uint8_t> b; put32(b, 1); x.add(1, kIdXstc, section("XSTC", b)); }
  { std::vector<uint8_t> b; put32(b, kTitle); put32(b, 1); x.add(1, kIdXthd, section("XTHD", b)); }
  x.add(3, 1, xstr({{1, "Armageddon Averted"}, {2, "Stopped the meteor storm"}, {3, "Finish Meteor Storm"},
                    {4, "Not that Super"}, {5, "Entered a cheat code"}, {6, "Enter a cheat code"}}));
  x.add(3, 3, xstr({{1, "Armageddon abgewendet"}, {2, "D2"}, {3, "D3"}, {4, "N4"}, {5, "D5"}, {6, "D6"}}));
  x.add(2, 0x10B, {0x89, 'P', 'N', 'G'});
  return x.build();
}

// An 'MC02' save file: `count` records a region; slot n's descriptor progress and region-n record-0 checksum.
std::vector<uint8_t> saveFile(const float progress[3], const uint32_t checksum[3], uint32_t count = 4) {
  std::vector<uint8_t> v;
  const uint32_t len1 = 88, len2 = 96 * count;
  put32(v, 0x4D433032u);
  put32(v, 28 + len1 + len2);
  put32(v, len1);
  put32(v, len2);
  put32(v, 0); put32(v, 0); put32(v, 0);
  v.resize(28 + 16, 0);
  for (int s = 0; s < 3; s++) {
    uint32_t bits = 0;
    std::memcpy(&bits, &progress[s], 4);
    put32(v, bits);
    v.resize(v.size() + 20, 0);
  }
  const size_t p2 = v.size();
  v.resize(p2 + len2, 0);
  for (int s = 0; s < 3; s++) {
    const size_t at = p2 + 16 * (size_t(s) * count) + 8;
    v[at] = uint8_t(checksum[s] >> 24); v[at + 1] = uint8_t(checksum[s] >> 16);
    v[at + 2] = uint8_t(checksum[s] >> 8); v[at + 3] = uint8_t(checksum[s]);
  }
  return v;
}
void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
void writeText(const fs::path& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << t;
}
std::string readText(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------------------------------- guest calls
uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;
constexpr uint32_t kArgs = 0x90780000u, kList = 0x90780100u, kOvl = 0x90780200u;
struct Ctx {
  PPCContext c{};
  explicit Ctx(uint32_t stack) {
    K::setGuestMemory(g_base, kGuestSize);
    c.r1.u32 = stack;
    c.r14.u32 = 0x829C0000u;
    K::setCurrentGuestContext(&c, offsetof(PPCContext, lr), offsetof(PPCContext, ctr), offsetof(PPCContext, r3),
                              offsetof(PPCContext, r1), offsetof(PPCContext, r31));
  }
  ~Ctx() { K::setCurrentGuestContext(nullptr, 0); }
};
uint32_t xmsg(uint32_t app, uint32_t msg, uint32_t ovl, uint32_t buf, uint32_t len) {
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  return K::ExportRegistry::instance().getExport("XMsgStartIORequest")(app, msg, ovl, buf, len, 0);
}
// The game's write: {count, ptr} -> {user, id}[count]
void stage(const std::vector<uint32_t>& ids, uint32_t count = 0xFFFFFFFFu, uint32_t ptr = kList) {
  K::writeGuestBE32(kArgs, count == 0xFFFFFFFFu ? uint32_t(ids.size()) : count);
  K::writeGuestBE32(kArgs + 4, ptr);
  for (size_t i = 0; i < ids.size(); i++) {
    K::writeGuestBE32(kList + uint32_t(8 * i), 0);
    K::writeGuestBE32(kList + uint32_t(8 * i) + 4, ids[i]);
  }
}
uint32_t write(const std::vector<uint32_t>& ids) {
  stage(ids);
  return xmsg(0xFB, 0x000B0008u, kOvl, kArgs, 8);
}
std::vector<uint8_t> guestBytes(uint32_t va, uint32_t n) {
  const uint8_t* p = K::guestVAToHost(va);
  return std::vector<uint8_t>(p, p + n);
}
bool hasIds(const A::Snapshot& s, std::vector<uint32_t> want) {
  if (s.unlocked.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); i++)
    if (s.unlocked[i].id != want[i]) return false;
  return true;
}

// A fake previous registration (D2): records its arguments, answers 0x1234.
uint32_t g_fakeArgs[6] = {}, g_fakeCalls = 0;
uint32_t fakePrev(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  g_fakeCalls++;
  const uint32_t a[6] = {r3, r4, r5, r6, r7, r8};
  std::memcpy(g_fakeArgs, a, sizeof a);
  return 0x1234u;
}
}  // namespace

int runAchievementsTest() {
  const char* gate = std::getenv("SR_ACHIEVEMENTS");
  std::printf("[ACHIEVEMENTS_TEST] the E14a core (gate SR_ACHIEVEMENTS=%s)\n", gate ? gate : "(unset: ON)");

  // ── T: the XDBF table ───────────────────────────────────────────────────────────────────────────────────────
  {
    const std::vector<uint8_t> db = fixtureTable();
    const A::Table t = A::parseTable(db, 0);
    check(t.ok && t.list.size() == 2 && t.titleId == kTitle && t.language == 1, "T1 the fixture: 2 rows, title, English (XSTC)");
    if (t.ok && t.list.size() == 2) {
      const A::Achievement& a = t.list[0];
      const A::Achievement& b = t.list[1];
      check(a.id == 11 && a.name == "Armageddon Averted" && a.unlockedText == "Stopped the meteor storm" &&
                a.lockedText == "Finish Meteor Storm" && a.gamerscore == 50 && a.imageId == 0x10B && !a.secret &&
                a.flags == 9,
            "T2 row 11: name / unlocked / locked text, 50 G, image, shown while locked");
      check(b.id == 12 && b.name == "Not that Super" && b.secret && b.gamerscore == 0,
            "T3 row 12: secret (the shown-while-locked flag clear), 0 G; the table order kept");
    }
    const A::Table de = A::parseTable(db, 3);
    check(de.ok && de.language == 3 && de.list[0].name == "Armageddon abgewendet", "T4 a wanted language with a table: German");
    const A::Table ko = A::parseTable(db, 7);
    check(ko.ok && ko.language == 1, "T5 a wanted language without a table: the XSTC default");
    const A::Table nd = A::parseTable(fixtureTable(false), 0);
    check(nd.ok && nd.language == 1, "T6 no XSTC: the first string table");
    std::vector<uint8_t> bad = db;
    bad[0] = 'Y';
    check(!A::parseTable(bad, 0).ok, "T7 not an XDBF: refused");
    check(!A::parseTable(std::vector<uint8_t>(db.begin(), db.begin() + 40), 0).ok, "T8 truncated: refused");
    XdbfBuilder nx;
    nx.add(1, kIdXach, xach({{11, 1, 2, 3, 0, 5, 8}}));
    check(!A::parseTable(nx.build(), 0).ok, "T9 no string table: refused");
    // every single-byte corruption parses or refuses without a crash (a smoke over the whole fixture)
    size_t refused = 0;
    for (size_t i = 0; i < db.size(); i++) {
      std::vector<uint8_t> m = db;
      m[i] ^= 0xFF;
      refused += A::parseTable(m, 0).ok ? 0 : 1;
    }
    std::printf("       T10: %zu of %zu one-byte corruptions refused, none crashed\n", refused, db.size());
    check(true, "T10 every one-byte corruption of the fixture parses or refuses (no crash)");
  }
  if (const char* real = std::getenv("SR_ACH_XDBF")) {
    std::ifstream f(real, std::ios::binary);
    const std::vector<uint8_t> db((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const A::Table t = A::parseTable(db, 0);
    uint32_t score = 0, secret = 0;
    for (const A::Achievement& a : t.list) { score += a.gamerscore; secret += a.secret ? 1 : 0; }
    std::printf("       R1: %s: ok=%d title=%08X rows=%zu G=%u secret=%u language=%u %s\n", real, t.ok ? 1 : 0, t.titleId,
                t.list.size(), score, secret, t.language, t.why.c_str());
    for (const A::Achievement& a : t.list)
      std::printf("         id %u %u G %s '%s'\n", a.id, a.gamerscore, a.secret ? "secret" : "shown", a.name.c_str());
    check(t.ok && t.titleId == kTitle && t.list.size() == 18 && score == 1000 && secret == 7,
          "R1 the real table: title 454107ED, 18 achievements, 1000 G, 7 secret");
  } else {
    std::printf("  skip R1 (SR_ACH_XDBF unset: the real table is checked live, [ACH] table line)\n");
  }

  // ── S: the sidecar codec and the save-file checksum ─────────────────────────────────────────────────────────
  {
    A::Sidecar sc;
    sc.titleId = kTitle;
    sc.slots[1].present = true;
    sc.slots[1].slotChecksum = 0xAABBCCDDu;
    sc.slots[1].unlocked = {{11, 1729670000000LL}, {12, 1729670000500LL}};
    const std::string j = A::sidecarJson(sc);
    A::Sidecar back;
    std::string why;
    const bool ok = A::parseSidecar(j, kTitle, &back, &why);
    check(ok && !back.slots[0].present && back.slots[1].present && !back.slots[2].present &&
              back.slots[1].slotChecksum == 0xAABBCCDDu && back.slots[1].unlocked.size() == 2 &&
              back.slots[1].unlocked[0].id == 11 && back.slots[1].unlocked[1].unixMs == 1729670000500LL,
          "S1 the sidecar round-trips (slots, checksum, ids, times)");
    const std::pair<const char*, std::string> bad[] = {
        {"S2 not JSON", "{garbage"},
        {"S3 version 2", "{\"v\": 2, \"title_id\": \"454107ED\", \"slots\": {}}"},
        {"S4 another title", "{\"v\": 1, \"title_id\": \"4D5307E6\", \"slots\": {}}"},
        {"S5 a slot key 3", "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"3\": null}}"},
        {"S6 a bad checksum", "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"0\": {\"slot_checksum\": \"xyz\", \"unlocked\": {}}}}"},
        {"S7 a bad id", "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"0\": {\"slot_checksum\": \"00000001\", \"unlocked\": {\"x\": 5}}}}"},
        {"S8 a negative time", "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"0\": {\"slot_checksum\": \"00000001\", \"unlocked\": {\"11\": -5}}}}"},
        {"S9 trailing bytes", j + "x"},
        {"S10 a duplicate key", "{\"v\": 1, \"v\": 1, \"title_id\": \"454107ED\", \"slots\": {}}"},
    };
    for (const auto& [name, text] : bad) {
      A::Sidecar o;
      std::string w;
      check(!A::parseSidecar(text, kTitle, &o, &w) && !w.empty(), name);
    }
    check(A::sidecarPath(fs::path("C:/r/X/T/00000001/Superman Returns")) ==
              fs::path("C:/r/X/T/00000001/Superman Returns.achievements.json"),
          "S11 the sidecar sits BESIDE the package folder");
    const float prog[3] = {0.25f, -1.0f, 0.5f};
    const uint32_t cs[3] = {0x11111111u, 0x22222222u, 0x33333333u};
    const std::vector<uint8_t> sv = saveFile(prog, cs);
    uint32_t c0 = 0, c1 = 0, c2 = 0;
    check(A::slotChecksumFromSave(sv, 0, &c0) && c0 == 0x11111111u && A::slotChecksumFromSave(sv, 2, &c2) &&
              c2 == 0x33333333u,
          "S12 a slot's stored checksum (region n, record 0, +8)");
    check(!A::slotChecksumFromSave(sv, 1, &c1), "S13 an empty slot (progress -1.0): no checksum");
    std::vector<uint8_t> bm = sv;
    bm[0] = 0;
    check(!A::slotChecksumFromSave(bm, 0, &c0) && !A::slotChecksumFromSave(std::vector<uint8_t>(sv.begin(), sv.begin() + 100), 0, &c0),
          "S14 not an MC02 container / truncated: no checksum");
    // S15 (the review's P2): one id, two spellings, would be two JSON keys; a later SAVE rewrites every slot and would
    // then write the same key twice (a file the next LOAD rejects whole). Only canonical decimal ids are accepted.
    A::Sidecar sa;
    std::string wa;
    const char* pre = "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"0\": {\"slot_checksum\": \"11111111\", \"unlocked\": {";
    const bool aliasRejected = !A::parseSidecar(std::string(pre) + "\"11\": 1, \"011\": 2}}}}", kTitle, &sa, &wa);
    const bool leadingZeroRejected = !A::parseSidecar(std::string(pre) + "\"011\": 2}}}}", kTitle, &sa, &wa);
    const bool zeroOk = A::parseSidecar(std::string(pre) + "\"0\": 2, \"11\": 1}}}}", kTitle, &sa, &wa) &&
                        sa.slots[0].unlocked.size() == 2;
    check(aliasRejected && leadingZeroRejected && zeroOk,
          "S15 sidecar ids are canonical decimal: '011' (alone or beside '11') makes the file unreadable; '0' and '11' parse");
  }

  // ── H: the hook, through the real export registry ───────────────────────────────────────────────────────────
  g_base = K::getGuestBase();
  K::registerStubs();
  Ctx mainCtx(0x90100000u);
  A::resetForTest();
  A::setTableForTest(fixtureTable());
  A::setClockForTest(1729670000000LL);
  {
    std::memset(K::guestVAToHost(kOvl), 0xCD, 0x1C);
    stage({11, 12});
    const std::vector<uint8_t> before = guestBytes(kArgs, 0x220);
    const uint64_t g0 = A::generation();
    const uint32_t rc = xmsg(0xFB, 0x000B0008u, kOvl, kArgs, 8);
    const A::Snapshot s = A::snapshot();
    std::printf("       H1: rc=%08X unlocked=%zu gen %llu -> %llu\n", rc, s.unlocked.size(), (unsigned long long)g0,
                (unsigned long long)s.generation);
    check(rc == 0 && guestBytes(kArgs, 0x220) == before, "H1 the guest sees today's answer: 0, no guest byte written");
    check(hasIds(s, {11, 12}) && s.unlocked[0].unixMs == 1729670000000LL && s.generation > g0 &&
              A::generation() == s.generation,
          "H2 both ids recorded with the time; the generation moved");
    A::setClockForTest(1729670009999LL);
    write({11, 99});
    const A::Snapshot s2 = A::snapshot();
    check(hasIds(s2, {11, 12}) && s2.unlocked[0].unixMs == 1729670000000LL,
          "H3 a repeated id keeps its first time; an id outside the XACH table is dropped");
    stage({}, 0);
    xmsg(0xFB, 0x000B0008u, 0, kArgs, 8);
    stage({}, 100000);
    xmsg(0xFB, 0x000B0008u, 0, kArgs, 8);
    stage({}, 1, 0x7FC80010u);   // into the no-access hardware reservation: refused, never read
    xmsg(0xFB, 0x000B0008u, 0, kArgs, 8);
    xmsg(0xFB, 0x000B0008u, 0, 0, 8);
    xmsg(0xFB, 0x000B0008u, 0, kArgs, 4);
    check(A::snapshot().unlocked.size() == 2, "H4 a zero / huge count, a hardware-window or null pointer, a short length: ignored");
    // a fake previous registration: the handler answers EXACTLY what it answers, and other traffic reaches it untouched
    K::ExportRegistry::instance().registerExport("XMsgStartIORequest", fakePrev);
    A::registerAchievementExports(K::ExportRegistry::instance());
    g_fakeCalls = 0;
    stage({12});
    const uint32_t r1 = xmsg(0xFB, 0x000B0008u, kOvl, kArgs, 8);
    const bool same1 = g_fakeCalls == 1 && g_fakeArgs[0] == 0xFB && g_fakeArgs[1] == 0x000B0008u &&
                       g_fakeArgs[2] == kOvl && g_fakeArgs[3] == kArgs && g_fakeArgs[4] == 8;
    const uint32_t r2 = xmsg(0xFB, 0x000B0009u, kOvl, kArgs, 8);
    const uint32_t r3 = xmsg(0xFA, 0x0007001Au, 0, kArgs, 12);
    check(r1 == 0x1234u && same1, "H5 the achievement write: the previous registration's own answer (0x1234), same arguments");
    check(r2 == 0x1234u && r3 == 0x1234u && g_fakeCalls == 3 && g_fakeArgs[0] == 0xFA,
          "H6 another XGI message and another app: passed to the previous registration");
    A::registerAchievementExports(K::ExportRegistry::instance());   // twice: never chains to itself
    g_fakeCalls = 0;
    xmsg(0xFB, 0x000B0009u, 0, kArgs, 8);
    check(g_fakeCalls == 1, "H7 registered twice: the previous registration is called once (no self-chain)");
    K::registerStubs();   // back to the real chain (the generic stub under the handler)
    // no table: writes are ignored, the read API says Unavailable
    A::resetForTest();
    A::setTableForTest({'n', 'o', 'p', 'e'});
    write({11});
    const A::Snapshot s3 = A::snapshot();
    check(s3.table == A::TableStatus::Unavailable && s3.list.empty() && s3.unlocked.empty(),
          "H8 an unreadable XDBF: TableStatus::Unavailable, nothing recorded");

    // H9 (the review's P1): isGuestVAInRange is an address bound only; a committed-but-NOACCESS (or uncommitted /
    // guard) page inside the reservation must not pass the hook's span check, so the hook never touches it.
    // One page of the scratch area is made NOACCESS (bracketed by the HRR removal protocol), then restored.
    A::resetForTest();
    A::setTableForTest(fixtureTable());
    // the page right after the scratch page (kArgs..kOvl share 0x90780000): made NOACCESS if committed; if it is not
    // committed it is already the uncommitted case the review names (either way: unreadable, inside the range)
    constexpr uint32_t kBadPage = 0x90781000u;
    uint8_t* bad = K::guestVAToHost(kBadPage);
    MEMORY_BASIC_INFORMATION mbi{};
    const bool committed = bad && VirtualQuery(bad, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT;
    DWORD oldProt = 0;
    K::hostRangeReadableBeginRemoval();
    const bool prot = committed && VirtualProtect(bad, 0x1000, PAGE_NOACCESS, &oldProt) != 0;
    K::hostRangeReadableEndRemoval();
    const bool unreadable = bad && (prot || !committed);
    const bool readableOk = A::guestSpanForTest(kArgs, 8);
    const bool inRange = K::isGuestVAInRange(kBadPage);   // the old check alone would have let it through
    const bool badRefused = unreadable && !A::guestSpanForTest(kBadPage, 8) && !A::guestSpanForTest(kBadPage - 4, 8);
    std::printf("       H9: committed=%d protected=%d inRange=%d readable(kArgs)=%d refused(the page, a straddling span)=%d\n",
                committed ? 1 : 0, prot ? 1 : 0, inRange ? 1 : 0, readableOk ? 1 : 0, badRefused ? 1 : 0);
    check(readableOk && inRange && badRefused,
          "H9a an unreadable page inside the guest range (and a span straddling into it) is not a readable span");
    if (badRefused) {   // only with the check in place: without it the hook would fault the process here
      K::writeGuestBE32(kArgs, 1);
      K::writeGuestBE32(kArgs + 4, kBadPage);
      const uint32_t rc = xmsg(0xFB, 0x000B0008u, kOvl, kArgs, 8);
      K::writeGuestBE32(kArgs + 4, kBadPage - 4);
      const uint32_t rc2 = xmsg(0xFB, 0x000B0008u, kOvl, kArgs, 8);
      check(rc == 0 && rc2 == 0 && A::snapshot().unlocked.empty(),
            "H9b the hook refuses an achievement array on a NOACCESS page: no fault, today's answer, nothing recorded");
    } else {
      check(false, "H9b not run: H9a failed (the hook would read the NOACCESS page)");
    }
    if (prot) {
      DWORD tmp = 0;
      const bool restored = VirtualProtect(bad, 0x1000, oldProt, &tmp) != 0 && A::guestSpanForTest(kBadPage, 8);
      K::hostRangeReadableInvalidate();
      check(restored, "H9c the page is restored readable (the rest of the suite never meets a NOACCESS page)");
      if (!restored) {   // the fix-check's P3: never run the later tests over a page that may still fault
        std::printf("[ACHIEVEMENTS_TEST] H9 could not restore page %08X: the suite stops here\n", kBadPage);
        A::resetForTest();
        std::printf("[ACHIEVEMENTS_TEST] %d passed, %d failed\n", g_pass, g_fail);
        return g_fail;
      }
    }
  }

  // ── E: tied to each save ────────────────────────────────────────────────────────────────────────────────────
  {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / ("sr_ach_test_" + std::to_string(std::rand()));
    const fs::path typeDir = root / "E0000000000000001" / "454107ED" / "00000001";
    const fs::path pkg = typeDir / "Superman Returns";
    fs::create_directories(pkg, ec);
    const float prog[3] = {0.25f, 0.5f, -1.0f};
    uint32_t cs[3] = {0xA0000000u, 0xA0000001u, 0};
    writeBytes(pkg / "Superman Returns", saveFile(prog, cs));
    const fs::path side = A::sidecarPath(pkg);
    A::resetForTest();
    A::setTableForTest(fixtureTable());
    A::setClockForTest(1729670000000LL);
    A::Snapshot s = A::snapshot();
    check(s.table == A::TableStatus::Ok && s.save == A::SaveStatus::NoSave && s.slot == -1 && s.unlocked.empty() &&
              s.list.size() == 2,
          "E1 boot: the table, NoSave, slot -1, nothing unlocked");
    A::notePackage(pkg);
    write({11});
    write({12});
    A::onSaveEvent("save", 1);
    s = A::snapshot();
    A::Sidecar sc;
    std::string why;
    const bool parsed = A::parseSidecar(readText(side), kTitle, &sc, &why);
    check(parsed && sc.slots[1].present && sc.slots[1].slotChecksum == 0xA0000001u && sc.slots[1].unlocked.size() == 2 &&
              !sc.slots[0].present && s.save == A::SaveStatus::Slot && s.slot == 1 && hasIds(s, {11, 12}),
          "E2 unlocks before the first save go into the slot at its SAVE (checksum from the save file); slot 1 bound");
    const std::string saveBytes = readText(pkg / "Superman Returns");
    const uint64_t gNew = A::generation();
    A::onSaveEvent("new", -1);
    s = A::snapshot();
    check(s.save == A::SaveStatus::NoSave && s.slot == -1 && s.unlocked.empty() && s.generation > gNew,
          "E3 NEW GAME clears the set and unbinds the slot");
    A::onSaveEvent("load", 1);
    s = A::snapshot();
    check(s.save == A::SaveStatus::Slot && s.slot == 1 && hasIds(s, {11, 12}), "E4 LOAD slot 1: its record");
    // E5 (tightened after the review): slot 1's record is set to {11} only, so the unsaved 12 is NOT in it - a union
    // of the session set and the record would keep 12 and fail; a replace drops it
    writeText(side, "{\"v\": 1, \"title_id\": \"454107ED\", \"slots\": {\"0\": null, \"1\": {\"slot_checksum\": \"A0000001\", "
                    "\"unlocked\": {\"11\": 1729670000000}}, \"2\": null}}");
    A::onSaveEvent("new", -1);
    write({12});
    A::onSaveEvent("load", 1);
    check(hasIds(A::snapshot(), {11}), "E5 LOAD replaces the set: an unsaved unlock (12, not in the record) is dropped");
    A::onSaveEvent("load", 0);
    s = A::snapshot();
    check(s.save == A::SaveStatus::Slot && s.slot == 0 && s.unlocked.empty(), "E6 LOAD of a slot with no record: empty");
    cs[1] = 0xBEEF0001u;   // slot 1 saved again by another play (a copied package): its checksum differs
    writeBytes(pkg / "Superman Returns", saveFile(prog, cs));
    A::onSaveEvent("load", 1);
    s = A::snapshot();
    check(s.save == A::SaveStatus::SlotUnreadable && s.slot == 1 && s.unlocked.empty(),
          "E7 a record for another save of the slot (checksum): ignored, empty, SlotUnreadable");
    const std::string saveBefore = readText(pkg / "Superman Returns");
    writeText(side, "{garbage");
    A::onSaveEvent("load", 1);
    s = A::snapshot();
    check(s.save == A::SaveStatus::SlotUnreadable && s.unlocked.empty() && readText(pkg / "Superman Returns") == saveBefore,
          "E8 a corrupt sidecar: ignored with a log line, empty, the save untouched");
    writeText(side, "{\"v\": 1, \"title_id\": \"4D5307E6\", \"slots\": {\"1\": {\"slot_checksum\": \"BEEF0001\", \"unlocked\": {\"11\": 1}}}}");
    A::onSaveEvent("load", 1);
    check(A::snapshot().save == A::SaveStatus::SlotUnreadable && A::snapshot().unlocked.empty(),
          "E9 another title's sidecar: ignored, empty");
    write({11});
    A::onSaveEvent("save", 0);   // over the foreign file: replaced whole
    A::Sidecar sc2;
    check(A::parseSidecar(readText(side), kTitle, &sc2, &why) && sc2.slots[0].present && sc2.slots[0].unlocked.size() == 1 &&
              sc2.slots[0].slotChecksum == 0xA0000000u && !sc2.slots[1].present,
          "E10 SAVE over an unreadable sidecar replaces it whole (valid again)");
    A::onSaveEvent("new", -1);
    write({12});
    A::onSaveEvent("save", 0);   // overwrite: slot 0's record follows the save that wrote it
    A::onSaveEvent("load", 0);
    check(hasIds(A::snapshot(), {12}), "E11 an overwrite of slot 0: the record follows the save ({12}, not {11})");
    fs::remove(side, ec);
    A::onSaveEvent("load", 1);
    s = A::snapshot();
    check(s.save == A::SaveStatus::Slot && s.unlocked.empty(), "E12 a missing sidecar: empty, Slot");
    A::onSaveEvent("load", 7);
    A::onSaveEvent("save", -1);
    A::onSaveEvent("overwrite", 1);
    A::onSaveEvent("query", 1);
    s = A::snapshot();
    check(s.slot == 1 && s.save == A::SaveStatus::Slot, "E13 a bad slot, overwrite and query change nothing");
    check(readText(pkg / "Superman Returns") == saveBefore && !saveBytes.empty(),
          "E14 the save file is never written by this unit (byte for byte)");

    // W (the review's P2): SR_SaveEvent through the REAL mod bridge (its slot tracker, then the achievements). The
    // achievements must act on the slot the EVENT names, never on the tracker's remembered slot.
    cs[0] = 0xA0000000u; cs[1] = 0xA0000001u;
    writeBytes(pkg / "Superman Returns", saveFile(prog, cs));
    const std::string saveW = readText(pkg / "Superman Returns");
    fs::remove(side, ec);
    A::resetForTest();
    A::setTableForTest(fixtureTable());
    A::setClockForTest(1729670000000LL);
    A::notePackage(pkg);
    K::modSlotResetForTest();
    auto saveEvent = [](const char* kind, int32_t sf) {
      K::modBridgeSetSaveFlowSlotForTest(sf);
      int32_t r = -9;
      (void)K::modBridgeDispatch("SR_SaveEvent", kind, &r);
    };
    write({11});
    saveEvent("save", 1);              // slot 1 := {11}
    write({12});                       // unsaved
    saveEvent("load", 7);              // an out-of-range +0x54: the tracker keeps slot 1, the event names no slot
    const A::Snapshot w1 = A::snapshot();
    check(hasIds(w1, {11, 12}) && w1.slot == 1,
          "W1 a LOAD whose +0x54 is out of range changes nothing (it does not reload the tracker's slot 1)");
    // W2 sets up its OWN state (the fix-check's P3: inheriting W1's let W1's old wrong reload mask the old SAVE bug):
    // slot 1's record {11}, the tracker on slot 1, the session set {11, 12}
    fs::remove(side, ec);
    A::resetForTest();
    A::setTableForTest(fixtureTable());
    A::setClockForTest(1729670000000LL);
    A::notePackage(pkg);
    K::modSlotResetForTest();
    write({11});
    saveEvent("save", 1);              // slot 1 := {11}; the tracker on slot 1
    write({12});                       // unsaved: the set is {11, 12}
    saveEvent("save", 3);              // +0x54 = 3 with no overwrite latch: the event names no slot
    A::Sidecar w;
    std::string wwhy;
    const bool wParsed = A::parseSidecar(readText(side), kTitle, &w, &wwhy);
    check(wParsed && w.slots[1].present && w.slots[1].unlocked.size() == 1 && w.slots[1].unlocked[0].id == 11 &&
              hasIds(A::snapshot(), {11, 12}),
          "W2 a SAVE naming no slot (+0x54 = 3, no latch) writes no record (slot 1 keeps {11}; never guessed)");
    saveEvent("overwrite", 0);
    saveEvent("save", 3);              // the overwrite latch names slot 0: its record := {11, 12}
    A::Sidecar w3;
    check(A::parseSidecar(readText(side), kTitle, &w3, &wwhy) && w3.slots[0].present && w3.slots[0].slotChecksum == 0xA0000000u &&
              w3.slots[0].unlocked.size() == 2 && w3.slots[1].unlocked.size() == 1 && readText(pkg / "Superman Returns") == saveW,
          "W3 an overwrite-confirmed SAVE (+0x54 = 3 after the latch) writes the latched slot; the save file untouched");
    K::modBridgeSetSaveFlowSlotForTest(-2);
    K::modSlotResetForTest();
    fs::remove_all(root, ec);
  }

  A::resetForTest();
  std::printf("[ACHIEVEMENTS_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
