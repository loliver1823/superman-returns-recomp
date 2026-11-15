// step5 E14a (lane B): the achievements core. Contract and rules: achievements.hpp.
#include "achievements.hpp"

#include "export_registry.hpp"
#include "guest_context.hpp"
#include "native_hardware_memory.hpp"
#include "xex_loader.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace sr::ach {
namespace {

// ------------------------------------------------------------------------------------------- the gate
bool achievementsOn() {   // default ON, =0 = the old generic stub stays and nothing is recorded or written
  static const bool v = [] { const char* e = std::getenv("SR_ACHIEVEMENTS"); return !e || e[0] != '0'; }();
  return v;
}

// ------------------------------------------------------------------------------------------- XDBF (Xenia xdbf_io.h)
constexpr uint32_t kXach = 0x58414348u;   // 'XACH'
constexpr uint32_t kXstc = 0x58535443u;   // 'XSTC'
constexpr uint32_t kXthd = 0x58544844u;   // 'XTHD'
constexpr uint32_t kXstr = 0x58535452u;   // 'XSTR'
constexpr uint16_t kSecMetadata = 1, kSecImage = 2, kSecStringTable = 3;   // kSecImage: lane E L-a (the icons)
constexpr uint32_t kXachEntry = 36;       // XdbfAchievementTableEntry (0x24)
constexpr uint32_t kShowUnachieved = 0x8; // "shown while locked"

uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Xdbf {
  const std::vector<uint8_t>* db = nullptr;
  uint64_t dataBase = 0, used = 0;
  bool open(const std::vector<uint8_t>& d, std::string* why) {
    constexpr uint64_t kHeader = 24, kEntry = 18, kFree = 8;
    if (d.size() < kHeader || std::memcmp(d.data(), "XDBF", 4) != 0) { *why = "not an XDBF database"; return false; }
    const uint64_t count = be32(d.data() + 8), usedN = be32(d.data() + 12), freeCount = be32(d.data() + 16);
    const uint64_t base = kHeader + count * kEntry + freeCount * kFree;
    if (usedN > count || base > d.size()) { *why = "XDBF header out of range"; return false; }
    db = &d;
    dataBase = base;
    used = usedN;
    return true;
  }
  // The bytes of entry (section, id).
  bool find(uint16_t section, uint64_t id, const uint8_t** out, uint32_t* size) const {
    const uint8_t* d = db->data();
    for (uint64_t i = 0; i < used; i++) {
      const uint8_t* e = d + 24 + i * 18;
      const uint64_t eid = (uint64_t(be32(e + 2)) << 32) | be32(e + 6);
      if (be16(e) != section || eid != id) continue;
      const uint64_t off = be32(e + 10), sz = be32(e + 14);
      if (sz == 0 || dataBase + off + sz > db->size()) return false;
      *out = d + dataBase + off;
      *size = uint32_t(sz);
      return true;
    }
    return false;
  }
  // The first string-table language present (0 if none).
  uint32_t firstLanguage() const {
    const uint8_t* d = db->data();
    for (uint64_t i = 0; i < used; i++) {
      const uint8_t* e = d + 24 + i * 18;
      if (be16(e) == kSecStringTable) return be32(e + 6);
    }
    return 0;
  }
};

// One XSTR table: {string id -> UTF-8}. False if absent or malformed.
bool loadStrings(const Xdbf& x, uint32_t language, std::map<uint16_t, std::string>* out) {
  const uint8_t* p = nullptr;
  uint32_t n = 0;
  if (!language || !x.find(kSecStringTable, language, &p, &n) || n < 14 || be32(p) != kXstr) return false;
  const uint32_t count = be16(p + 12);
  uint64_t at = 14;   // 64-bit: at + len can never wrap, whatever the entry size (the fix-check P3)
  std::map<uint16_t, std::string> m;
  for (uint32_t i = 0; i < count; i++) {
    if (at + 4 > n) return false;
    const uint16_t id = be16(p + at), len = be16(p + at + 2);
    at += 4;
    if (at + len > n) return false;
    m[id] = std::string(reinterpret_cast<const char*>(p + at), len);
    at += len;
  }
  *out = std::move(m);
  return true;
}

// ------------------------------------------------------------------------------------------- a small JSON reader
// Just enough for the sidecar (objects, strings without escapes beyond \" \\ \/, integers, null, true / false).
struct Json {
  enum Kind { Null, Bool, Num, Str, Obj } kind = Null;
  bool b = false;
  long long n = 0;
  std::string s;
  std::map<std::string, std::shared_ptr<Json>> o;
};
struct JsonParser {
  const std::string& t;
  size_t i = 0;
  int depth = 0;
  explicit JsonParser(const std::string& text) : t(text) {}
  void ws() { while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) i++; }
  bool str(std::string* out) {
    if (i >= t.size() || t[i] != '"') return false;
    i++;
    std::string r;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c == '\\') {
        if (i >= t.size()) return false;
        c = t[i++];
        if (c != '"' && c != '\\' && c != '/') return false;
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
      r.push_back(c);
    }
    if (i >= t.size()) return false;
    i++;
    *out = std::move(r);
    return true;
  }
  bool value(Json* v) {
    if (++depth > 8) return false;
    ws();
    if (i >= t.size()) return false;
    bool ok = false;
    const char c = t[i];
    if (c == '{') {
      i++;
      v->kind = Json::Obj;
      ws();
      if (i < t.size() && t[i] == '}') { i++; ok = true; }
      else {
        while (true) {
          ws();
          std::string k;
          if (!str(&k)) break;
          ws();
          if (i >= t.size() || t[i] != ':') break;
          i++;
          auto child = std::make_shared<Json>();
          if (!value(child.get()) || v->o.count(k)) break;   // a duplicate key is malformed
          v->o[k] = child;
          ws();
          if (i < t.size() && t[i] == ',') { i++; continue; }
          if (i < t.size() && t[i] == '}') { i++; ok = true; }
          break;
        }
      }
    } else if (c == '"') {
      v->kind = Json::Str;
      ok = str(&v->s);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const size_t s0 = i;
      if (t[i] == '-') i++;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') i++;
      const std::string digits = t.substr(s0, i - s0);
      if (digits.empty() || digits == "-" || digits.size() > 19) ok = false;
      else { v->kind = Json::Num; v->n = std::strtoll(digits.c_str(), nullptr, 10); ok = true; }
    } else if (t.compare(i, 4, "null") == 0) { i += 4; v->kind = Json::Null; ok = true; }
    else if (t.compare(i, 4, "true") == 0) { i += 4; v->kind = Json::Bool; v->b = true; ok = true; }
    else if (t.compare(i, 5, "false") == 0) { i += 5; v->kind = Json::Bool; ok = true; }
    depth--;
    return ok;
  }
};
bool parseHex32(const std::string& s, uint32_t* out) {
  if (s.size() != 8) return false;
  uint32_t v = 0;
  for (char c : s) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (d < 0) return false;
    v = (v << 4) | uint32_t(d);
  }
  *out = v;
  return true;
}
std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}

// ------------------------------------------------------------------------------------------- state
struct State {
  std::mutex mu;
  Table table;
  bool tableFailed = false;             // the XDBF was there but unreadable: logged once, not retried
  std::vector<uint8_t> tableOverride;   // the test seam
  bool haveOverride = false;
  uint32_t language = 0;                // the XGetLanguage answer (a generic stub: 0)
  int64_t clockMs = 0;                  // test seam: 0 = the real clock
  std::vector<Unlock> set;              // the session set, in unlock order
  SaveStatus save = SaveStatus::NoSave;
  int slot = -1;
  fs::path pkg;
};
State& st() {
  static State s;
  return s;
}
std::atomic<uint64_t> g_gen{0};
kernel::ExportFn g_prev = nullptr;

int64_t nowMs(const State& s) {
  if (s.clockMs) return s.clockMs;
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Caller holds st().mu. True once the table is readable.
bool ensureTableLocked(State& s) {
  if (s.table.ok) return true;
  if (s.tableFailed) return false;
  std::vector<uint8_t> db;
  if (s.haveOverride) db = s.tableOverride;
  else if (!kernel::xexTitleDatabase(db)) return false;   // not loaded yet (or no XDBF): tried again later
  Table t = parseTable(db, s.language);
  if (!t.ok) {
    s.tableFailed = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] the title's XDBF is unreadable (%s): no achievements this session",
                t.why.c_str());
    return false;
  }
  uint32_t score = 0, secret = 0;
  for (const Achievement& a : t.list) { score += a.gamerscore; secret += a.secret ? 1 : 0; }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] table: title %08X, %zu achievements, %u G, %u secret, language %u",
              t.titleId, t.list.size(), score, secret, t.language);
  s.table = std::move(t);
  g_gen.fetch_add(1, std::memory_order_acq_rel);
  return true;
}
const Achievement* findLocked(const State& s, uint32_t id) {
  for (const Achievement& a : s.table.list)
    if (a.id == id) return &a;
  return nullptr;
}
bool inSet(const std::vector<Unlock>& v, uint32_t id) {
  return std::any_of(v.begin(), v.end(), [id](const Unlock& u) { return u.id == id; });
}

bool readFile(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return bool(f) || f.eof();
}
// The temp file, then a rename over the old one: a reader never sees half a sidecar.
bool writeAtomic(const fs::path& p, const std::string& text, std::string* why) {
  const fs::path tmp = p.parent_path() / (p.filename().string() + ".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { *why = "cannot create " + tmp.generic_string(); return false; }
    f << text;
    f.flush();
    if (!f) { *why = "cannot write " + tmp.generic_string(); return false; }
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    *why = "cannot replace " + p.generic_string();
    return false;
  }
  return true;
}
bool saveFileChecksum(const fs::path& pkg, int slot, uint32_t* out) {
  std::string bytes;
  if (pkg.empty() || !readFile(pkg / "Superman Returns", &bytes)) return false;
  const std::vector<uint8_t> v(bytes.begin(), bytes.end());
  return slotChecksumFromSave(v, slot, out);
}

// ------------------------------------------------------------------------------------------- the hook
bool guestSpan(uint32_t va, uint32_t len) {
  if (va == 0 || len == 0 || uint64_t(va) + len > 0x100000000ull) return false;
  if (!kernel::isGuestVAInRange(va) || !kernel::isGuestVAInRange(va + len - 1)) return false;
  // never into the no-access hardware reservation (a release build protects it: a read there faults)
  const uint64_t hw0 = kernel::kHardwareMemoryBase, hw1 = hw0 + kernel::kHardwareMemoryBytes;
  if (!(uint64_t(va) + len <= hw0 || va >= hw1)) return false;
  // isGuestVAInRange is an address-space bound only (guest_context.hpp): an uncommitted or guard page passes it
  // and then FAULTS. The span must map contiguously and every page it touches must be committed and readable
  // (the review's P1). This runs on the GUEST thread, twice per achievement write (unlocks only: the game's
  // XUserWriteAchievements); an uncached VirtualQuery walk of the guest reservation costs 5-10 ms each, so it uses
  // the interval-cached predicate (guest_context.hpp hostRangeReadableCached: the same answer, the HRR removal
  // protocol keeps it exact; the E14a fix-check follow-up).
  const uint8_t* h = kernel::guestVAToHost(va);
  if (!h || kernel::guestVAToHost(va + len - 1) != h + (len - 1)) return false;
  return kernel::hostRangeReadableCached(h, len);
}

// The repeat lines (a re-write of an unlocked id, a dropped or unreadable write) are capped: the game may re-send
// its writes; the first unlock of each id is always logged (at most the table's size).
std::atomic<uint32_t> g_repeatLines{0};
bool repeatLineDue() { return g_repeatLines.fetch_add(1, std::memory_order_relaxed) < 32; }

void recordWrite(uint32_t buf, uint32_t len) {
  if (len < 8 || !guestSpan(buf, 8)) {
    if (repeatLineDue())
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] write with an unreadable argument block %08X (length %u): ignored",
                  buf, len);
    return;
  }
  const uint32_t count = kernel::readGuestU32BE(buf), ptr = kernel::readGuestU32BE(buf + 4);
  if (count == 0 || count > 64 || !guestSpan(ptr, count * 8)) {
    if (repeatLineDue())
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] write of %u achievement(s) at %08X: unreadable, ignored", count, ptr);
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> ids;   // {user, id}
  for (uint32_t i = 0; i < count; i++)
    ids.emplace_back(kernel::readGuestU32BE(ptr + 8 * i), kernel::readGuestU32BE(ptr + 8 * i + 4));
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  const bool table = ensureTableLocked(s);
  for (const auto& [user, id] : ids) {
    if (!table) {
      if (repeatLineDue()) SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] write id=%u user=%u ignored: no table", id, user);
      continue;
    }
    const Achievement* a = findLocked(s, id);
    if (!a) {
      if (repeatLineDue())
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] write id=%u user=%u is not in the XACH table: dropped", id, user);
      continue;
    }
    if (inSet(s.set, id)) {
      if (repeatLineDue())
        SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] write id=%u user=%u '%s': already unlocked", id, user,
                    a->name.c_str());
      continue;
    }
    s.set.push_back({id, nowMs(s)});
    g_gen.fetch_add(1, std::memory_order_acq_rel);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] write id=%u user=%u '%s' (%u G): unlocked, %zu of %zu, slot %d", id,
                user, a->name.c_str(), a->gamerscore, s.set.size(), s.table.list.size(), s.slot);
  }
}

uint32_t exp_XMsgStartIORequest(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  // XGI (0xFB) XUserWriteAchievements (0x000B0008): record, then answer exactly as before
  if (r3 == 0xFBu && r4 == 0x000B0008u) recordWrite(r6, r7);
  return g_prev ? g_prev(r3, r4, r5, r6, r7, r8) : 0u;
}

std::string slotsText(const Sidecar& sc) {
  std::string r;
  for (int i = 0; i < 3; i++)
    r += (i ? " " : "") + std::to_string(i) + "=" + (sc.slots[i].present ? std::to_string(sc.slots[i].unlocked.size()) : "-");
  return r;
}

}  // namespace

// ------------------------------------------------------------------------------------------- pure parts
Table parseTable(const std::vector<uint8_t>& db, uint32_t wantLanguage) {
  Table t;
  Xdbf x;
  if (!x.open(db, &t.why)) return t;
  const uint8_t* p = nullptr;
  uint32_t n = 0;
  if (!x.find(kSecMetadata, kXach, &p, &n) || n < 14 || be32(p) != kXach) { t.why = "no XACH section"; return t; }
  const uint32_t count = be16(p + 12);
  if (14ull + uint64_t(count) * kXachEntry > n) { t.why = "the XACH section is truncated"; return t; }
  uint32_t def = 0;
  const uint8_t* q = nullptr;
  uint32_t m = 0;
  if (x.find(kSecMetadata, kXstc, &q, &m) && m >= 16 && be32(q) == kXstc) def = be32(q + 12);
  if (x.find(kSecMetadata, kXthd, &q, &m) && m >= 16 && be32(q) == kXthd) t.titleId = be32(q + 12);
  std::map<uint16_t, std::string> strings;
  for (uint32_t lang : {wantLanguage, def, x.firstLanguage()}) {
    if (loadStrings(x, lang, &strings)) { t.language = lang; break; }
  }
  if (!t.language) { t.why = "no readable string table"; return t; }
  auto text = [&strings](uint16_t id) {
    const auto it = strings.find(id);
    return it == strings.end() ? std::string() : it->second;
  };
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* e = p + 14 + i * kXachEntry;
    Achievement a;
    a.id = be16(e);
    a.name = text(be16(e + 2));
    a.unlockedText = text(be16(e + 4));
    a.lockedText = text(be16(e + 6));
    a.imageId = be32(e + 8);
    a.gamerscore = be16(e + 12);
    a.flags = be32(e + 16);
    a.secret = (a.flags & kShowUnachieved) == 0;
    t.list.push_back(std::move(a));
  }
  t.ok = true;
  return t;
}

bool parseSidecar(const std::string& text, uint32_t titleId, Sidecar* out, std::string* why) {
  Json root;
  JsonParser jp(text);
  if (!jp.value(&root)) { *why = "not JSON"; return false; }
  jp.ws();
  if (jp.i != text.size()) { *why = "trailing bytes after the JSON"; return false; }
  if (root.kind != Json::Obj) { *why = "not a JSON object"; return false; }
  auto get = [](const Json& o, const char* k) -> const Json* {
    const auto it = o.o.find(k);
    return it == o.o.end() ? nullptr : it->second.get();
  };
  const Json* v = get(root, "v");
  if (!v || v->kind != Json::Num || v->n != 1) { *why = "not version 1"; return false; }
  const Json* tid = get(root, "title_id");
  uint32_t t = 0;
  if (!tid || tid->kind != Json::Str || !parseHex32(tid->s, &t)) { *why = "no title_id"; return false; }
  if (t != titleId) { *why = "another title's sidecar (" + tid->s + ")"; return false; }
  const Json* slots = get(root, "slots");
  if (!slots || slots->kind != Json::Obj) { *why = "no slots object"; return false; }
  Sidecar sc;
  sc.titleId = t;
  for (const auto& [k, val] : slots->o) {
    if (k != "0" && k != "1" && k != "2") { *why = "a slot key '" + k + "'"; return false; }
    SlotRecord& r = sc.slots[k[0] - '0'];
    if (val->kind == Json::Null) continue;
    if (val->kind != Json::Obj) { *why = "slot " + k + " is not an object"; return false; }
    const Json* cs = get(*val, "slot_checksum");
    if (!cs || cs->kind != Json::Str || !parseHex32(cs->s, &r.slotChecksum)) { *why = "slot " + k + ": no slot_checksum"; return false; }
    const Json* un = get(*val, "unlocked");
    if (!un || un->kind != Json::Obj) { *why = "slot " + k + ": no unlocked object"; return false; }
    for (const auto& [ids, ms] : un->o) {
      // canonical decimal only ("11", never "011"): two spellings of one id would be two JSON keys, and a later
      // SAVE (which rewrites every slot) would then write the same key twice - a file the next LOAD rejects whole
      if (ids.empty() || ids.size() > 5 || (ids.size() > 1 && ids[0] == '0') ||
          !std::all_of(ids.begin(), ids.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        *why = "slot " + k + ": an achievement id '" + ids + "'";
        return false;
      }
      if (ms->kind != Json::Num || ms->n < 0) { *why = "slot " + k + ": a bad unlock time"; return false; }
      const uint32_t id = uint32_t(std::strtoul(ids.c_str(), nullptr, 10));
      if (std::any_of(r.unlocked.begin(), r.unlocked.end(), [&](const Unlock& u) { return u.id == id; })) {
        *why = "slot " + k + ": achievement id " + ids + " twice";
        return false;
      }
      r.unlocked.push_back({id, int64_t(ms->n)});
    }
    std::sort(r.unlocked.begin(), r.unlocked.end(),
              [](const Unlock& a, const Unlock& b) { return a.unixMs != b.unixMs ? a.unixMs < b.unixMs : a.id < b.id; });
    r.present = true;
  }
  *out = std::move(sc);
  return true;
}

std::string sidecarJson(const Sidecar& s) {
  std::string j = "{\"v\": 1, \"title_id\": \"" + hex32(s.titleId) + "\", \"slots\": {";
  for (int i = 0; i < 3; i++) {
    j += std::string(i ? ", " : "") + "\"" + std::to_string(i) + "\": ";
    const SlotRecord& r = s.slots[i];
    if (!r.present) { j += "null"; continue; }
    j += "{\"slot_checksum\": \"" + hex32(r.slotChecksum) + "\", \"unlocked\": {";
    for (size_t k = 0; k < r.unlocked.size(); k++)
      j += std::string(k ? ", " : "") + "\"" + std::to_string(r.unlocked[k].id) + "\": " +
           std::to_string(r.unlocked[k].unixMs);
    j += "}}";
  }
  j += "}}\n";
  return j;
}

fs::path sidecarPath(const fs::path& pkg) {
  fs::path p = pkg;
  if (!p.has_filename()) p = p.parent_path();   // a trailing separator
  return p.parent_path() / (p.filename().string() + ".achievements.json");
}

bool slotChecksumFromSave(const std::vector<uint8_t>& f, int slot, uint32_t* out) {
  if (slot < 0 || slot > 2 || f.size() < 28 || be32(f.data()) != 0x4D433032u) return false;   // 'MC02'
  const uint64_t total = be32(f.data() + 4), len1 = be32(f.data() + 8), len2 = be32(f.data() + 12);
  if (len1 != 88 || len2 == 0 || len2 % 96 != 0 || total != 28 + len1 + len2 || f.size() < total) return false;
  const uint64_t count = len2 / 96;
  uint32_t bits = be32(f.data() + 28 + 16 + 24 * slot);   // the slot descriptor's f32 progress
  float progress = 0;
  std::memcpy(&progress, &bits, 4);
  if (!(progress > -1.0f)) return false;                   // an empty slot (-1.0)
  *out = be32(f.data() + 28 + len1 + 16 * (uint64_t(slot) * count) + 8);   // region n, record 0, +8
  return true;
}

// ------------------------------------------------------------------------------------------- the read API
Snapshot snapshot() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  Snapshot r;
  r.table = ensureTableLocked(s) ? TableStatus::Ok : TableStatus::Unavailable;
  r.save = s.save;
  r.slot = s.save == SaveStatus::NoSave ? -1 : s.slot;
  if (r.table == TableStatus::Ok) {
    r.list = s.table.list;
    r.unlocked = s.set;
  }
  r.generation = g_gen.load(std::memory_order_acquire);
  return r;
}
uint64_t generation() { return g_gen.load(std::memory_order_acquire); }

// lane E L-a: the icons (achievements.hpp iconPng / imageFromXdbf)
bool imageFromXdbf(const std::vector<uint8_t>& xdbf, uint32_t imageId, std::vector<uint8_t>* png) {
  Xdbf x;
  std::string why;
  if (!png || !x.open(xdbf, &why)) return false;
  const uint8_t* p = nullptr;
  uint32_t n = 0;
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (!x.find(kSecImage, imageId, &p, &n) || n < 8 || std::memcmp(p, kPng, 8) != 0) return false;
  png->assign(p, p + n);
  return true;
}

bool iconPng(uint32_t imageId, std::vector<uint8_t>* png) {
  std::vector<uint8_t> db;
  bool seam = false;
  {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.haveOverride) {   // the test seam is then the only source, as for the table
      db = s.tableOverride;
      seam = true;
    }
  }
  if (!seam && !kernel::xexTitleDatabase(db)) return false;
  return imageFromXdbf(db, imageId, png);
}

// ------------------------------------------------------------------------------------------- the kernel wiring
void registerAchievementExports(kernel::ExportRegistry& reg) {
  if (!achievementsOn()) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] off (SR_ACHIEVEMENTS=0): XMsgStartIORequest keeps its old answer");
    return;
  }
  const kernel::ExportFn cur = reg.getExport("XMsgStartIORequest");
  if (cur != exp_XMsgStartIORequest) g_prev = cur;   // registerStubs runs twice: never chain to ourselves
  reg.registerExport("XMsgStartIORequest", exp_XMsgStartIORequest);
}

void notePackage(const fs::path& pkg) {
  if (!achievementsOn()) return;
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.pkg != pkg)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] save package \"%s\" (sidecar \"%s\")", pkg.generic_string().c_str(),
                sidecarPath(pkg).generic_string().c_str());
  s.pkg = pkg;
}

void onSaveEvent(const std::string& kind, int slot) {
  if (!achievementsOn() || (kind != "new" && kind != "load" && kind != "save")) return;
  State& s = st();
  if (kind == "new") {
    std::lock_guard<std::mutex> lk(s.mu);
    const size_t had = s.set.size();
    s.set.clear();
    s.save = SaveStatus::NoSave;
    s.slot = -1;
    g_gen.fetch_add(1, std::memory_order_acq_rel);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ACH] NEW GAME: the set cleared (%zu dropped), no slot", had);
    return;
  }
  if (slot < 0 || slot > 2) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] %s with no slot (%d): nothing changed", kind.c_str(), slot);
    return;
  }
  // the I/O happens outside the lock (snapshot() is never blocked on a disk)
  fs::path pkg;
  uint32_t titleId = 0;
  bool table = false;
  std::vector<Unlock> set;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    pkg = s.pkg;
    table = ensureTableLocked(s);
    titleId = s.table.titleId;
    set = s.set;
  }
  const fs::path side = pkg.empty() ? fs::path() : sidecarPath(pkg);
  Sidecar sc;
  sc.titleId = titleId;
  std::string text, why;
  bool readable = false, exists = false;
  if (!side.empty()) {
    std::error_code ec;
    exists = fs::exists(side, ec) && !ec;
    if (exists && readFile(side, &text)) readable = table && parseSidecar(text, titleId, &sc, &why);
    if (exists && !readable && why.empty()) why = table ? "unreadable file" : "no table to check it against";
  }
  uint32_t checksum = 0;
  const bool haveChecksum = saveFileChecksum(pkg, slot, &checksum);
  if (kind == "load") {
    SaveStatus status = SaveStatus::Slot;
    std::vector<Unlock> loaded;
    const char* how = "no sidecar: empty";
    if (pkg.empty()) how = "no save package seen: empty";
    else if (exists && !readable) { status = SaveStatus::SlotUnreadable; how = "the sidecar is IGNORED: empty"; }
    else if (readable && sc.slots[slot].present) {
      if (!haveChecksum || sc.slots[slot].slotChecksum != checksum) {
        status = SaveStatus::SlotUnreadable;
        how = "the record is for another save of this slot (checksum): IGNORED, empty";
      } else {
        how = "the slot's record";
        std::lock_guard<std::mutex> lk(s.mu);
        for (const Unlock& u : sc.slots[slot].unlocked)
          if (findLocked(s, u.id) && !inSet(loaded, u.id)) loaded.push_back(u);
      }
    } else if (readable) how = "no record for the slot: empty";
    std::lock_guard<std::mutex> lk(s.mu);
    const size_t dropped = s.set.size();
    s.set = loaded;
    s.save = status;
    s.slot = slot;
    g_gen.fetch_add(1, std::memory_order_acq_rel);
    SR_LOG_BOOT(status == SaveStatus::Slot ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[ACH] LOAD slot %d: %s (%zu unlocked; %zu session unlock(s) replaced)%s%s", slot, how, loaded.size(),
                dropped, why.empty() ? "" : "; ", why.c_str());
    return;
  }
  // SAVE: the slot's record := the current set, with the slot's checksum from the file the game just wrote
  {
    std::lock_guard<std::mutex> lk(s.mu);
    s.save = SaveStatus::Slot;
    s.slot = slot;
    g_gen.fetch_add(1, std::memory_order_acq_rel);
  }
  if (pkg.empty() || !table || !titleId || !haveChecksum) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] SAVE slot %d: the sidecar is NOT written (%s); %zu unlocked stay in memory",
                slot, pkg.empty() ? "no save package seen" : !table ? "no table" : !titleId ? "no title id" : "the save file's slot is unreadable",
                set.size());
    return;
  }
  if (exists && !readable)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACH] SAVE slot %d: the old sidecar was unreadable (%s): it is replaced", slot,
                why.c_str());
  SlotRecord& r = sc.slots[slot];
  r.present = true;
  r.slotChecksum = checksum;
  r.unlocked = set;
  std::string werr;
  const bool ok = writeAtomic(side, sidecarJson(sc), &werr);
  SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[ACH] SAVE slot %d: %zu unlocked, checksum %08X -> \"%s\" %s (slots %s)", slot, set.size(), checksum,
              side.generic_string().c_str(), ok ? "written" : ("NOT written: " + werr).c_str(), slotsText(sc).c_str());
}

// ------------------------------------------------------------------------------------------- test seams
bool guestSpanForTest(uint32_t va, uint32_t len) { return guestSpan(va, len); }
void resetForTest() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.table = Table();
  s.tableFailed = false;
  s.tableOverride.clear();
  s.haveOverride = false;
  s.language = 0;
  s.clockMs = 0;
  s.set.clear();
  s.save = SaveStatus::NoSave;
  s.slot = -1;
  s.pkg.clear();
  g_gen.fetch_add(1, std::memory_order_acq_rel);
}
void setTableForTest(const std::vector<uint8_t>& xdbf) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.tableOverride = xdbf;
  s.haveOverride = true;
  s.table = Table();
  s.tableFailed = false;
}
void setLanguageForTest(uint32_t language) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.language = language;
  s.table = Table();
  s.tableFailed = false;
}
void setClockForTest(int64_t unixMs) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.clockMs = unixMs;
}

}  // namespace sr::ach
