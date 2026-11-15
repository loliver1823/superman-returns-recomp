// iter-463 phase 2: the level-skip executor core. Contract and provenance:
// devtools_skip_core.hpp. Every function below mirrors tools/devtools/skipexec.py (the
// reference) and tools/devtools/frida_skip.js; the comments name the reference function.
#include "devtools_skip_core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sr {
namespace devtools {
namespace skip {

// ============================================================== JSON ====
J& J::set(const std::string& k, J v) {
  for (auto& kv : o_)
    if (kv.first == k) { kv.second = std::move(v); return kv.second; }
  o_.emplace_back(k, std::move(v));
  return o_.back().second;
}

const J* J::get(const std::string& k) const {
  if (t_ != T::Obj) return nullptr;
  for (const auto& kv : o_)
    if (kv.first == k) return &kv.second;
  return nullptr;
}

static void dumpStr(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04X", c);
          out += b;
        } else {
          out += static_cast<char>(c);   // UTF-8 passes through
        }
    }
  }
  out += '"';
}

static void dumpTo(std::string& out, const J& j) {
  char b[40];
  switch (j.type()) {
    case J::T::Null: out += "null"; break;
    case J::T::Bool: out += j.b() ? "true" : "false"; break;
    case J::T::Int: std::snprintf(b, sizeof b, "%lld", static_cast<long long>(j.i())); out += b; break;
    case J::T::Dbl: std::snprintf(b, sizeof b, "%.17g", j.d()); out += b; break;
    case J::T::Str: dumpStr(out, j.s()); break;
    case J::T::Arr: {
      out += '[';
      bool first = true;
      for (const auto& x : j.a()) {
        if (!first) out += ',';
        first = false;
        dumpTo(out, x);
      }
      out += ']';
      break;
    }
    case J::T::Obj: {
      out += '{';
      bool first = true;
      for (const auto& kv : j.o()) {
        if (!first) out += ',';
        first = false;
        dumpStr(out, kv.first);
        out += ':';
        dumpTo(out, kv.second);
      }
      out += '}';
      break;
    }
  }
}

std::string J::dump() const {
  std::string out;
  dumpTo(out, *this);
  return out;
}

// ============================================================== helpers ====
std::string h8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

static std::string hexBytes(const std::vector<uint8_t>& b) {
  std::string s = "0x";
  char x[4];
  for (uint8_t c : b) {
    std::snprintf(x, sizeof x, "%02X", c);
    s += x;
  }
  return s;
}

static std::string latin1ToUtf8(const std::string& b) {
  std::string out;
  for (unsigned char c : b) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t fnv1(const std::string& bytes) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char c : bytes) h = (h * 16777619u) ^ c;
  return h;
}

uint32_t hash31(const std::string& bytes) {
  uint32_t h = 0;
  for (unsigned char c : bytes) h = 31u * h + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(c)));
  return h;
}

static J optU32(const std::optional<uint32_t>& v) { return v ? J::num(*v) : J::null(); }
static J optStr(const std::optional<std::string>& v) { return v ? J::str(*v) : J::null(); }

// field access on a trigger record (frida_skip.js semantics: absent = undefined)
static const J* fld(const J& rec, const char* k) { return rec.get(k); }
static bool fldIs(const J& rec, const char* k, int64_t v) {
  const J* x = rec.get(k);
  return x && x->isInt() && x->i() == v;
}
static bool truthy(const J* x) {
  if (!x) return false;
  switch (x->type()) {
    case J::T::Null: return false;
    case J::T::Bool: return x->b();
    case J::T::Int: return x->i() != 0;
    case J::T::Dbl: return x->d() != 0;
    case J::T::Str: return !x->s().empty();
    default: return true;
  }
}
static bool sameStr(const J* x, const std::string& s) { return x && x->isStr() && x->s() == s; }

// ============================================================== memory ====
const std::optional<std::vector<uint8_t>>& Mem::page(uint32_t pg) {
  auto it = cache_.find(pg);
  if (it != cache_.end()) return it->second;
  PageView v = src_(pg);      // may throw Miss: nothing is cached then
  std::optional<std::vector<uint8_t>> p;
  if (v.p && v.len) p.emplace(v.p, v.p + std::min<uint32_t>(v.len, 4096));
  return cache_.emplace(pg, std::move(p)).first->second;
}

std::vector<uint8_t> Mem::raw(uint32_t va, uint32_t n) {
  std::vector<uint8_t> out(n);
  uint32_t got = 0;
  while (got < n) {
    const uint32_t a = va + got, pg = a >> 12, off = a & 0xFFFu;
    const auto& p = page(pg);
    if (!p || off >= p->size()) throw ReadErr{"unreadable " + h8(va) + "+" + std::to_string(n)};
    const uint32_t take = std::min<uint32_t>(n - got, static_cast<uint32_t>(p->size()) - off);
    std::memcpy(out.data() + got, p->data() + off, take);
    got += take;
    if (got < n && off + take < 4096) throw ReadErr{"unreadable " + h8(va) + "+" + std::to_string(n)};
  }
  return out;
}

uint8_t Mem::u8(uint32_t va) { return raw(va, 1)[0]; }

uint32_t Mem::u32(uint32_t va) {
  const auto b = raw(va, 4);
  return be32(b.data());
}

std::optional<std::string> Mem::cstr(uint32_t va, uint32_t maxlen) {
  if (!va) return std::nullopt;
  std::string out;
  while (out.size() < maxlen) {
    const uint32_t a = va + static_cast<uint32_t>(out.size()), pg = a >> 12, off = a & 0xFFFu;
    const auto& p = page(pg);
    if (!p || off >= p->size()) throw ReadErr{"unterminated/unreadable string at " + h8(va)};
    const uint8_t c = (*p)[off];
    if (c == 0) return out;
    out += static_cast<char>(c);
  }
  throw ReadErr{"string at " + h8(va) + " longer than " + std::to_string(maxlen)};
}

// ============================================================== ported lookups ====
uint32_t World::vecFind(uint32_t vec, uint32_t key) {
  Mem& m = m_;
  uint32_t v3 = m.u32(vec);
  const uint32_t end = m.u32(vec + 4);
  uint32_t v4 = end;
  if (end == v3) return end;
  for (int i = 0; i < 64; i++) {
    const uint32_t mid = v3 + 4u * ((v4 - v3) >> 3);
    const uint32_t k = m.u32(m.u32(mid));
    if (k == key) return mid;
    if (k <= key) v3 = mid + 4; else v4 = mid;
    if (v4 == v3) return end;
  }
  throw ReadErr{"vec_find did not converge at " + h8(vec) + " (not a sorted vector?)"};
}

uint32_t World::resolvePath(uint32_t container, const std::string& path) {
  Mem& m = m_;
  uint32_t a1 = container;
  size_t v3 = 0, v5 = 0;
  if (!path.empty()) {
    bool broke = false;
    while (a1) {
      if (path[v5] == '/') {
        const uint32_t it = vecFind(a1, fnv1(path.substr(v3, v5 - v3)));
        a1 = it == m.u32(a1 + 4) ? 0 : m.u32(m.u32(it) + 4);
        v3 = v5 + 1;
      }
      v5 += 1;
      if (v5 >= path.size()) { broke = true; break; }
    }
    if (!broke) return 0;
  }
  if (a1) {
    const uint32_t it2 = vecFind(a1, fnv1(path.substr(std::min(v3, path.size()))));
    if (it2 != m.u32(a1 + 4)) return m.u32(it2);
  }
  return 0;
}

uint32_t World::dasNode(uint32_t handle, const std::string& name) {
  if (!handle) return 0;
  const uint32_t c = m_.u32(handle + C::DAS_CONTAINER);
  if (!c) return 0;
  return resolvePath(c, name);
}

std::optional<std::string> World::atom(uint32_t key) {
  auto it = atomCache_.find(key);
  if (it != atomCache_.end()) return it->second;
  Mem& m = m_;
  const uint32_t holder = m.u32(C::G_ATOMS);
  if (!holder) throw ReadErr{"atom map 0x829A23B8 is null"};
  const uint32_t tree = m.u32(holder), header = tree + 4;
  uint32_t x = m.u32(tree + 12), res = header;
  int i;
  for (i = 0; i < 96; i++) {
    if (!x) break;
    if (m.u32(x + 16) < key) x = m.u32(x);
    else { res = x; x = m.u32(x + 4); }
  }
  if (i == 96) throw ReadErr{"atom tree walk did not terminate"};
  std::optional<std::string> out;
  if (!(res == header || key < m.u32(res + 16))) out = m.cstr(m.u32(res + 28));
  atomCache_.emplace(key, out);
  return out;
}

std::optional<std::string> World::dasStr(uint32_t handle, const std::string& name) {
  const uint32_t n = dasNode(handle, name);
  if (!n) return std::nullopt;
  const auto s = atom(m_.u32(n + C::NODE_VALUE));
  if (!s) return std::nullopt;
  return latin1ToUtf8(*s);
}

uint32_t World::flowMgr() { return m_.u32(C::G_WORLDFLOW_MGR); }

uint32_t World::currentFlow() {
  Mem& m = m_;
  const uint32_t mgr = flowMgr();
  if (!mgr) return 0;
  const auto target = m.cstr(m.u32(8u * m.u32(mgr + C::WFM_CUR_DEF) + m.u32(mgr + C::WFM_DEFS) + 4u));
  const uint32_t slots = m.u32(mgr + C::WFM_SLOTS);
  for (uint32_t i = 0; i < 8; i++) {
    const uint32_t f = m.u32(slots + 4 * i);
    if (f && target && m.cstr(f + C::FLOW_NAME) == target) return f;
  }
  return 0;
}

std::vector<uint32_t> World::statList() {
  Mem& m = m_;
  const uint32_t st = m.u32(C::G_STATS);
  if (!st) return {};
  const uint32_t arr = m.u32(st), n = m.u32(st + 4);
  if (n > 4096) throw ReadErr{"stat count " + std::to_string(n) + " implausible"};
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < n; i++) out.push_back(m.u32(arr + 4 * i));
  return out;
}

uint32_t World::findStat(const std::string& name) {
  Mem& m = m_;
  if (!m.u32(C::G_STATS) || name.empty()) return 0;
  for (uint32_t e : statList()) {
    const std::string s = m.cstr(m.u32(e + C::STAT_NAME)).value_or("");
    if (s.substr(0, name.size()) == name) return e;    // strncmp(name, s, strlen(name)) == 0
  }
  return 0;
}

std::optional<uint32_t> World::decodeStatCap(uint32_t e) {
  Mem& m = m_;
  const uint32_t fn = m.u32(m.u32(e) + C::STAT_SETCAP_VTSLOT);
  if (m.u32(fn) == 0x4E800020u) return std::nullopt;
  const auto b = m.raw(fn, 24);
  uint32_t w[6];
  for (int i = 0; i < 6; i++) w[i] = be32(b.data() + 4 * i);
  if (w[0] == 0xC8030008u && (w[1] & 0xFFFF0000u) == 0xD8230000u && w[2] == 0xFF000800u &&
      w[3] == 0x4C990020u && w[4] == 0xD8230008u && w[5] == 0x4E800020u) {
    const uint32_t off = w[1] & 0xFFFF;
    (void)m.raw(e + off, 8);                 // objstate reads the cap here (m.f64)
    return off;
  }
  throw UnknownErr{"stat SetStatCap slot " + h8(fn) + " has an unsupported shape"};
}

uint32_t World::getComponent(uint32_t obj, uint32_t fourcc) {
  Mem& m = m_;
  const uint32_t fn = m.u32(m.u32(obj) + 36);
  uint32_t pc = fn;
  for (int i = 0; i < 16; i++) {
    const auto b = m.raw(pc, 16);
    const uint32_t w0 = be32(&b[0]), w1 = be32(&b[4]), w2 = be32(&b[8]), w3 = be32(&b[12]);
    if (w0 == 0x38600000u && w1 == 0x4E800020u) return 0;
    if ((w0 & 0xFFFF0000u) == 0x3D600000u && (w1 & 0xFFFF0000u) == 0x616B0000u && w2 == 0x7F045800u &&
        w3 == 0x4D9A0020u) {
      if ((((w0 & 0xFFFF) << 16) | (w1 & 0xFFFF)) == fourcc) return obj;
      pc += 16;
      continue;
    }
    throw UnknownErr{"vt+36 getter " + h8(fn) + " has an unsupported shape"};
  }
  throw UnknownErr{"vt+36 getter " + h8(fn) + " too long"};
}

uint32_t World::hashLookup(uint32_t tbl, const std::string& tok) {
  Mem& m = m_;
  const uint32_t k = hash31(tok), size = m.u32(tbl + 8);
  if (size == 0) throw ReadErr{"filter name table size 0 (the game traps: twllei)"};
  const uint32_t keys = m.u32(tbl + 24), vals = m.u32(tbl + 12);
  uint32_t v8 = k % size, v6 = 1, v7 = 0;
  while (m.u32(keys + 4 * v8) != k) {
    const uint32_t v9 = v6 + v8;
    v7 += 1;
    v6 *= 2;
    v8 = v9 % size;
    if (v7 >= 10) return 0;
  }
  return m.u32(vals + 4 * v8);
}

std::vector<uint8_t> World::buildMask(uint32_t nametbl, const std::string& names) {
  Mem& m = m_;
  std::vector<uint8_t> mask(32, 0);
  size_t a = 0;
  for (;;) {
    const size_t bar = names.find('|', a);
    const std::string tok = names.substr(a, bar == std::string::npos ? std::string::npos : bar - a);
    if (!tok.empty()) {
      const uint32_t p = hashLookup(nametbl + 84, tok);
      const uint32_t row = p ? (m.u32(p) * 48u + m.u32(nametbl + 72)) : nametbl + 40;
      const auto r = m.raw(row, 32);
      for (int i = 0; i < 32; i++) mask[i] |= r[i];
    }
    if (bar == std::string::npos) break;
    a = bar + 1;
  }
  return mask;
}

uint32_t World::findFilter(uint32_t filterset, const std::vector<uint8_t>& mask) {
  Mem& m = m_;
  const uint32_t h = filterset + 0x14, buckets = m.u32(h + 4), n = m.u32(h + 8);
  if (n == 0) throw ReadErr{"filter hash bucket count 0 (the game traps)"};
  const std::string key(mask.begin(), mask.end());
  uint32_t node = m.u32(buckets + 4 * (fnv1(key) % n));
  for (int i = 0; i < 4096; i++) {
    if (!node) return 0;
    const uint32_t v = m.u32(node);
    if (m.raw(v, 32) == mask) return v;
    node = m.u32(node + 4);
  }
  throw ReadErr{"filter chain did not terminate"};
}

std::vector<uint32_t> World::filterMembers(uint32_t filt) {
  Mem& m = m_;
  const uint32_t b = m.u32(filt + 64), e = m.u32(filt + 68), n = (e - b) >> 2;
  if (n > 65536) throw ReadErr{"filter member count " + std::to_string(n) + " implausible"};
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < n; i++) out.push_back(m.u32(b + 4 * i));
  return out;
}

// ============================================================== World ====
std::pair<uint32_t, uint32_t> World::base() {
  if (!flowMgr()) throw Refusal{"no_flow_mgr", ""};
  const uint32_t cur = currentFlow();
  if (!cur) throw Refusal{"no_current_flow", ""};
  const auto nm = m_.cstr(cur + C::FLOW_NAME);
  if (!nm || *nm != "BaseWorldFlow") throw Refusal{"current_flow_not_base", nm ? latin1ToUtf8(*nm) : "None"};
  if (m_.u32(cur + C::FLOW_NAME_HASH) != hash31(*nm)) throw Refusal{"flow_hash", ""};
  const uint32_t h = m_.u32(cur + C::FLOW_DAS);
  if (!h) throw Refusal{"no_das", ""};
  return {cur, h};
}

std::optional<std::tuple<uint32_t, uint32_t, uint32_t>> World::das(const std::string& name) {
  const uint32_t h = base().second;
  const uint32_t n = dasNode(h, name);
  if (!n) return std::nullopt;
  const uint32_t v = m_.u32(n + C::NODE_VALUE);
  const uint32_t t = (m_.u32(n + C::NODE_FLAGS) >> 17) & 0x7F;
  return std::make_tuple(n, v, t);
}

std::optional<uint32_t> World::dasVal(const std::string& name) {
  const auto d = das(name);
  if (!d) return std::nullopt;
  return std::get<1>(*d);
}

bool World::dasSorted() {
  // skipexec.das_sorted: every key is read first, then compared
  const uint32_t h = base().second;
  const uint32_t c = m_.u32(h + C::DAS_CONTAINER), b = m_.u32(c), e = m_.u32(c + 4);
  const uint32_t n = (e - b) >> 2;
  if (n > 20000) throw Refusal{"das_unsorted", "count " + std::to_string(n)};
  std::vector<uint32_t> keys;
  for (uint32_t i = 0; i < n; i++) keys.push_back(m_.u32(m_.u32(b + 4 * i)));
  for (size_t i = 0; i + 1 < keys.size(); i++)
    if (!(keys[i] < keys[i + 1])) return false;
  return true;
}

std::vector<int64_t> World::wonBits(const std::string& prefix) {
  std::vector<int64_t> out;
  for (int i = 0; i < 9; i++) {
    const auto v = dasVal(prefix + std::to_string(i));
    if (!v) continue;
    for (int b = 0; b < 32; b++)
      if ((*v >> b) & 1) out.push_back(32 * i + b);
  }
  return out;
}

bool World::won(int64_t bit) {
  const auto v = dasVal("objectiveStatus" + std::to_string(bit >> 5));
  return v && ((*v >> (bit & 31)) & 1);
}

std::vector<std::string> World::loaded() {
  const uint32_t mgr = flowMgr();
  if (!mgr) return {};
  const uint32_t sl = m_.u32(mgr + C::WFM_SLOTS);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < 8; i++) {
    const uint32_t f = m_.u32(sl + 4 * i);
    if (f) out.push_back(latin1ToUtf8(m_.cstr(f + C::FLOW_NAME).value_or("")));
  }
  return out;
}

const J& World::triggers() {
  if (trig_) return *trig_;
  Mem& m = m_;
  J out = J::arr();
  const uint32_t reg = m.u32(C::G_FILTER_REGISTRY);
  if (!reg) {
    trig_ = out;
    return *trig_;
  }
  const auto nm = m.cstr(m.u32(C::OFF_NAME_OBJ_TRIGGERS));
  if (!nm || *nm != "OBJ|TRIGGERS") throw Refusal{"filter_name", nm ? latin1ToUtf8(*nm) : "null"};
  const uint32_t nametbl = m.u32(reg);
  const auto mask = buildMask(nametbl, *nm);
  const auto emask = buildMask(nametbl, "ENGAGED");
  const uint32_t filt = findFilter(m.u32(reg + 8), mask);
  if (filt) {
    for (uint32_t mem : filterMembers(filt)) {
      if (!mem) continue;
      J rec = J::obj();
      rec.set("member", J::str(h8(mem)));
      try {
        triggerRec(mem, rec, emask);
      } catch (const ReadErr&) {
        rec.set("error", J::str("unreadable"));
      } catch (const UnknownErr&) {
        rec.set("error", J::str("unknown"));
      }
      out.push(std::move(rec));
    }
  }
  trig_ = std::move(out);
  return *trig_;
}

void World::triggerRec(uint32_t mem, J& rec, const std::vector<uint8_t>& emask) {
  Mem& m = m_;
  const uint32_t actor = getComponent(mem, C::FOURCC_ACTR);
  rec.set("actor", J::str(h8(actor)));
  if (!actor) return;
  const uint32_t h = m.u32(actor + C::ACTOR_DAS);
  rec.set("das", J::str(h8(h)));
  const uint32_t n = h ? dasNode(h, "objectiveIndex") : 0;
  rec.set("idx", n ? J::num(m.u32(n + C::NODE_VALUE)) : J::null());
  rec.set("flow", h ? optStr(dasStr(h, "objectiveWorldFlowName")) : J::null());
  rec.set("logical", h ? optStr(dasStr(h, "triggerLogicalOperator")) : J::null());
  rec.set("power", h ? optStr(dasStr(h, "triggerPower")) : J::null());
  const uint32_t tn = h ? dasNode(h, "triggerDistance") : 0;
  std::optional<uint32_t> ad;
  if (tn) {
    const uint32_t t = (m.u32(tn + C::NODE_FLAGS) >> 17) & 0x7F, v = m.u32(tn + C::NODE_VALUE);
    if (t == C::T_FLOAT) {
      ad = v;
    } else if (t == C::T_INT) {
      const float f = static_cast<float>(static_cast<double>(static_cast<int32_t>(v)));
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      ad = bits;
    }
  }
  rec.set("attr_distance", ad ? J::str(h8(*ad)) : J::null());
  const auto g = m.raw(actor + C::ACTOR_GROUPS, 32);
  bool any = false, all = true;
  for (int i = 0; i < 32; i++) {
    if (emask[i]) any = true;
    if ((g[i] & emask[i]) != emask[i]) all = false;
  }
  rec.set("engaged", J::boolean(any && all));
  const uint32_t brain = m.u32(actor + C::ACTOR_BRAIN);
  rec.set("state", J::null());
  if (brain && m.u32(brain + C::BRAIN_ACTOR) == actor) {
    const uint32_t mach = m.u32(brain + C::BRAIN_MACHINE);
    const uint32_t st = mach ? m.u32(mach) : 0;
    if (st) {
      rec.set("state", J::str(h8(st)));
      const bool vt = m.u32(st) == C::VT_TRIGGER_STATE;
      rec.set("vt_ok", J::boolean(vt));
      if (vt) {
        rec.set("flags", J::num(m.u32(st + C::STATE_FLAGS)));
        rec.set("distance", J::str(h8(m.u32(st + C::STATE_DISTANCE))));
        rec.set("event", J::num(m.u32(st + C::STATE_EVENT)));
      }
    }
  }
}

bool World::triggerCond(int64_t idx, const std::string& what) {
  for (const J& t : triggers().a()) {
    if (!fldIs(t, "idx", idx) || !truthy(fld(t, "vt_ok"))) continue;
    if (what == "available" && !(fld(t, "flags")->i() & 1)) return true;
    if (what == "engaged" && truthy(fld(t, "engaged"))) return true;
  }
  return false;
}

J World::baseFsm() {
  Mem& m = m_;
  const uint32_t flow = base().first;
  const uint32_t fsm = m.u32(flow + C::FLOW_FSM);
  J out = J::obj();
  out.set("fsm", J::str(h8(fsm)));
  out.set("index", J::null());
  out.set("state_id", J::null());
  out.set("verified", J::boolean(false));
  if (!fsm || m.u32(fsm) != C::VT_FLOW_FSM) return out;
  const int32_t cur = static_cast<int32_t>(m.u32(fsm + C::FSM_CUR));
  const uint32_t n = m.u32(fsm + C::FSM_COUNT), arr = m.u32(fsm + C::FSM_STATES);
  out.set("index", J::num(cur));
  const Graph* g = graph_;
  if (!g || g->ids.size() != n || n > 4096) return out;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t s = m.u32(arr + 4 * i), tn = m.u32(s + C::FSTATE_NTRANS), ta = m.u32(s + C::FSTATE_TRANS);
    if (tn > 256) return out;
    std::vector<int64_t> live;
    for (uint32_t k = 0; k < tn; k++) live.push_back(m.u32(m.u32(ta + 4 * k) + C::FTRANS_DEST));
    if (live != g->trans[i]) return out;
  }
  out.set("verified", J::boolean(true));
  out.set("state_id", (cur >= 0 && static_cast<uint32_t>(cur) < n) ? J::num(g->ids[cur]) : J::null());
  return out;
}

// ---- iter-466 map lane: public read helpers (kernel/map_markers.cpp) ----
std::vector<uint32_t> World::filterMemberList(const std::string& names) {
  Mem& m = m_;
  const uint32_t reg = m.u32(C::G_FILTER_REGISTRY);
  if (!reg) return {};
  const uint32_t filt = findFilter(m.u32(reg + 8), buildMask(m.u32(reg), names));
  return filt ? filterMembers(filt) : std::vector<uint32_t>{};
}

std::optional<double> World::statValue(const std::string& name) {
  Mem& m = m_;
  const uint32_t e = findStat(name);
  if (!e) return std::nullopt;
  const uint32_t fn = m.u32(m.u32(e) + C::STAT_VALUE_VTSLOT);
  const auto b = m.raw(fn, 8);
  const uint32_t ins = be32(&b[0]), ret = be32(&b[4]);
  if (!(ret == 0x4E800020u && (ins & 0xFFFF0000u) == 0xC8230000u)) return std::nullopt;  // lfd f1,off(r3); blr
  const auto v = m.raw(e + (ins & 0xFFFF), 8);
  uint64_t bits = 0;
  for (int i = 0; i < 8; i++) bits = (bits << 8) | v[i];
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

std::optional<uint32_t> World::flowDasByName(const std::string& name) {
  const uint32_t mgr = flowMgr();
  if (!mgr) return std::nullopt;
  const uint32_t sl = m_.u32(mgr + C::WFM_SLOTS);
  for (uint32_t i = 0; i < 8; i++) {
    const uint32_t f = m_.u32(sl + 4 * i);
    if (f && latin1ToUtf8(m_.cstr(f + C::FLOW_NAME).value_or("")) == name) {
      const uint32_t h = m_.u32(f + C::FLOW_DAS);
      if (h) return h;
    }
  }
  return std::nullopt;
}

J World::xp() {
  Mem& m = m_;
  const uint32_t e = findStat("CurrentXP");
  if (!e) throw Refusal{"no_stat", "CurrentXP"};
  const uint32_t fn = m.u32(m.u32(e) + C::STAT_VALUE_VTSLOT);
  const auto b = m.raw(fn, 8);
  const uint32_t ins = be32(&b[0]), ret = be32(&b[4]);
  if (!(ret == 0x4E800020u && (ins & 0xFFFF0000u) == 0xC8230000u)) throw Refusal{"stat_shape", "getter " + h8(fn)};
  const uint32_t voff = ins & 0xFFFF;
  const auto cap = decodeStatCap(e);
  J out = J::obj();
  out.set("stat", J::str(h8(e)));
  out.set("value_off", J::num(voff));
  out.set("value", J::str(hexBytes(m.raw(e + voff, 8))));
  out.set("cap_off", cap ? J::num(*cap) : J::null());
  out.set("cap", J::null());
  if (cap) out.set("cap", J::str(hexBytes(m.raw(e + *cap, 8))));
  return out;
}

std::optional<uint32_t> World::runState() {
  const uint32_t p = m_.u32(C::G_RUN_STATE_OBJ);
  if (!p) return std::nullopt;
  return m_.u32(p + C::RUN_STATE_OFF);
}

// objstate.failure_verdict (live run 1, open item 15)
static J failureVerdict(const J& stateId, std::optional<uint32_t> activated, std::optional<uint32_t> mini,
                        const Graph* g) {
  std::string kind;
  if (g && g->hasFail && stateId.isInt()) {
    const uint32_t sid = static_cast<uint32_t>(stateId.i());
    auto in = [&](const std::vector<uint32_t>& v) { return std::find(v.begin(), v.end(), sid) != v.end(); };
    if (in(g->failGameOver)) kind = "game_over";
    else if (in(g->failObjective))
      kind = !mini ? "loss_mini_unread" : (*mini == 0 ? "objective_loss" : "metro_event_loss");
  }
  J out = J::obj();
  out.set("state_id", stateId);
  out.set("kind", kind.empty() ? J::null() : J::str(kind));
  out.set("objective_failed",
          J::boolean((kind == "objective_loss" || kind == "game_over") && activated && *activated == 0));
  return out;
}

J World::failure() {
  J bf = baseFsm();
  const J sid = truthy(bf.get("verified")) ? *bf.get("state_id") : J::null();
  const auto act = dasVal("objectiveActivated");
  const auto mini = dasVal("miniEventEngaged");
  return failureVerdict(sid, act, mini, graph_);
}

bool World::pastPromptPoint() {
  try {
    J bf = baseFsm();
    if (!truthy(bf.get("verified"))) return false;
    const J* s = bf.get("state_id");
    if (!(s->isInt() && (s->i() == C::STATE_TRIGGERS || s->i() == C::STATE_IN_OBJECTIVE))) return false;
    const auto rs = runState();
    return !(rs && *rs == C::RUN_STATE_POPUP);
  } catch (const Refusal&) {
    return false;
  } catch (const ReadErr&) {
    return false;
  } catch (const UnknownErr&) {
    return false;
  }
}

bool World::runningIndex(int64_t idx, const std::optional<std::string>& flow) {
  try {
    const auto act = dasVal("objectiveActivated");
    if (!act || *act != 1) return false;
    const auto bi = dasVal("objectiveBitIndex");
    if (!bi || *bi != static_cast<uint32_t>(idx)) return false;
    if (flow) return dasStr(base().second, "objectiveWorldFlowName") == flow;
    return true;
  } catch (const Refusal&) {
    return false;
  } catch (const ReadErr&) {
    return false;
  } catch (const UnknownErr&) {
    return false;
  }
}

J World::storyKey() {
  try {
    J st = J::arr(), fd = J::arr();
    for (int i = 0; i < 9; i++) st.push(optU32(dasVal("objectiveStatus" + std::to_string(i))));
    for (int i = 0; i < 9; i++) fd.push(optU32(dasVal("objectiveFound" + std::to_string(i))));
    J xpv;
    try {
      xpv = *xp().get("value");
    } catch (const Refusal&) {
      xpv = J::null();
    }
    std::vector<int64_t> eng;
    for (const J& t : triggers().a()) {
      const J* ix = fld(t, "idx");
      if (truthy(fld(t, "engaged")) && ix && ix->isInt()) eng.push_back(ix->i());
    }
    std::sort(eng.begin(), eng.end());
    J out = J::arr();
    out.push(std::move(st));
    out.push(std::move(fd));
    out.push(optU32(dasVal("currentHeroPointChapter")));
    out.push(std::move(xpv));
    out.push(optU32(dasVal("GoToNextChapter")));
    out.push(optU32(dasVal("objectiveActivated")));
    out.push(optU32(dasVal("objectiveWorldFlowName")));
    J ld = J::arr();
    for (const auto& s : loaded()) ld.push(J::str(s));
    out.push(std::move(ld));
    J e = J::arr();
    for (int64_t x : eng) e.push(J::num(x));
    out.push(std::move(e));
    return out;
  } catch (const Refusal&) {
    return J::null();
  } catch (const ReadErr&) {
    return J::null();
  } catch (const UnknownErr&) {
    return J::null();
  }
}

static std::string asciiLower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool World::cond(const Cond& c) {
  try {
    if (c.kind == "das") {
      const auto v = dasVal(*c.name);
      return v && *v == static_cast<uint32_t>(*c.value & 0xFFFFFFFF);
    }
    if (c.kind == "won") return won(*c.value);
    if (c.kind == "flow_loaded") {
      const std::string want = asciiLower(*c.name);
      for (const auto& x : loaded())
        if (asciiLower(x) == want) return true;
      return false;
    }
    if (c.kind == "trigger") return triggerCond(*c.value, *c.name);
    if (c.kind == "base_state") {
      J bf = baseFsm();
      const J* s = bf.get("state_id");
      return s->isInt() && s->i() == *c.value;
    }
    if (c.kind == "pad_prompt") {
      const auto rs = runState();
      return rs && *rs == C::RUN_STATE_POPUP;
    }
    if (c.kind == "objective_failed") return failure().get("objective_failed")->b();
  } catch (const Refusal&) {
    return false;
  } catch (const ReadErr&) {
    return false;
  } catch (const UnknownErr&) {
    return false;
  }
  throw UnknownErr{"cond kind " + c.kind + " is not a pure read"};
}

static J writeRec(uint32_t va, int width, const std::string& old, const std::string& nw, const std::string& what) {
  J w = J::obj();
  w.set("va", J::str(h8(va)));
  w.set("width", J::num(width));
  w.set("old", J::str(old));
  w.set("new", J::str(nw));
  w.set("what", J::str(what));
  return w;
}

static std::string h2(uint32_t v) {
  char b[8];
  std::snprintf(b, sizeof b, "0x%02X", v & 0xFFu);
  return b;
}

// ---- DT-2 dev-tool state (skipexec.World._rb_next / timers / minievents / metro) ----
uint32_t World::rbNext(uint32_t n) {    // Eastl_RBTreeIncrement_sub_8271D578
  const uint32_t r = m_.u32(n + C::TN_RIGHT);
  if (r) {
    uint32_t lft = m_.u32(r + C::TN_LEFT);
    if (!lft) return r;
    for (uint32_t k = 0; k < C::RB_MAX_DEPTH; k++) {
      const uint32_t res = lft;
      lft = m_.u32(lft + C::TN_LEFT);
      if (!lft) return res;
    }
    throw Refusal{"timer_map_shape", "left chain deeper than 64"};
  }
  uint32_t i = m_.u32(n + C::TN_PARENT);
  uint32_t k = 0;
  for (; k < C::RB_MAX_DEPTH; k++) {
    if (n != m_.u32(i + C::TN_RIGHT)) break;
    n = i;
    i = m_.u32(i + C::TN_PARENT);
  }
  if (k == C::RB_MAX_DEPTH) throw Refusal{"timer_map_shape", "parent chain deeper than 64"};
  return m_.u32(n + C::TN_RIGHT) != i ? i : n;
}

J World::timers() {
  const uint32_t mgr = m_.u32(C::G_TIMER_MGR);
  const uint32_t mp = mgr ? m_.u32(mgr) : 0;
  if (!mp) throw Refusal{"no_timer_mgr", ""};
  const uint32_t count = m_.u32(mp + C::MAP_COUNT);
  if (count > C::MAX_TIMERS) throw Refusal{"timer_map_shape", "count " + std::to_string(count)};
  const uint32_t anchor = mp + C::MAP_ANCHOR;
  uint32_t n = m_.u32(mp + C::MAP_BEGIN);
  J nodes = J::arr();
  while (n != anchor) {
    if (!n || nodes.size() >= count)
      throw Refusal{"timer_map_shape", "the walk passed the count " + std::to_string(count)};
    J rec = J::obj();
    rec.set("node", J::str(h8(n)));
    rec.set("owner", J::str(h8(m_.u32(n + C::TN_OWNER))));
    rec.set("hash", J::str(h8(m_.u32(n + C::TN_HASH))));
    rec.set("flow", J::num(m_.u8(n + C::TN_FLOW)));
    rec.set("run", J::num(m_.u8(n + C::TN_RUN)));
    nodes.push(std::move(rec));
    n = rbNext(n);
  }
  if (nodes.size() != count)
    throw Refusal{"timer_map_shape", "walked " + std::to_string(nodes.size()) + " of " + std::to_string(count)};
  J out = J::obj();
  out.set("mgr", J::str(h8(mgr)));
  out.set("map", J::str(h8(mp)));
  out.set("count", J::num(count));
  out.set("nodes", std::move(nodes));
  return out;
}

J World::minievents() {
  const uint32_t mgr = m_.u32(C::G_OBJECTIVE_MGR);
  if (!mgr) throw Refusal{"no_objective_mgr", ""};
  J out = J::obj();
  out.set("mgr", J::str(h8(mgr)));
  out.set("enabled", J::num(m_.u8(mgr + C::OBJ_MINIEVENTS)));
  return out;
}

J World::metro() {
  const uint32_t o = m_.u32(C::G_METRO);
  if (!o) throw Refusal{"no_metro", ""};
  J out = J::obj();
  out.set("obj", J::str(h8(o)));
  out.set("health", J::num(m_.u32(o + C::METRO_HEALTH)));
  out.set("max", J::num(m_.u32(o + C::METRO_MAX)));
  out.set("frac", J::str(h8(m_.u32(o + C::METRO_FRAC))));
  out.set("damage", J::num(m_.u8(o + C::METRO_DAMAGE)));
  out.set("lock", J::num(m_.u8(o + C::METRO_LOCK)));
  return out;
}

J World::cheatsReport() {
  J out = J::obj();
  const std::pair<const char*, J (World::*)()> parts[] = {
      {"timers", &World::timers}, {"minievents", &World::minievents}, {"metro", &World::metro}};
  for (const auto& p : parts) {
    J v;
    try {
      v = (this->*p.second)();
    } catch (const Refusal& e) {
      v = J::obj();
      v.set("refused", J::str(e.code));
    } catch (const ReadErr&) {
      v = J::obj();
      v.set("refused", J::str("unreadable"));
    } catch (const UnknownErr&) {
      v = J::obj();
      v.set("refused", J::str("unknown"));
    }
    out.set(p.first, std::move(v));
  }
  return out;
}

static uint32_t hexU32(const J* x) { return static_cast<uint32_t>(std::stoul(x->s(), nullptr, 16)); }

J World::resolveCheat(const std::string& prim, const std::string& arg, const CheatState* cheats) {
  J out = J::arr();
  if (prim == "pause_timers") {
    const J t = timers();
    if (arg == "on") {
      for (const J& n : t.get("nodes")->a()) {
        if (n.get("flow")->i() && n.get("run")->i())
          out.push(writeRec(hexU32(n.get("node")) + C::TN_RUN, 1, h2(static_cast<uint32_t>(n.get("run")->i())), h2(0),
                            "pause_timers on: timer " + n.get("hash")->s() + " of " + n.get("owner")->s() + " +37 := 0"));
      }
      return out;
    }
    if (!cheats) return out;
    for (const J& rec : cheats->timers.a()) {
      for (const J& n : t.get("nodes")->a()) {
        if (n.get("node")->s() == rec.get("node")->s() && n.get("owner")->s() == rec.get("owner")->s() &&
            n.get("hash")->s() == rec.get("hash")->s()) {
          if (n.get("run")->i() == 0)
            out.push(writeRec(hexU32(n.get("node")) + C::TN_RUN, 1, h2(0), h2(1),
                              "pause_timers off: timer " + n.get("hash")->s() + " of " + n.get("owner")->s() +
                                  " +37 := 1"));
          break;
        }
      }
    }
    return out;
  }
  if (prim == "minievents") {
    const J me = minievents();
    const int64_t nv = arg == "on" ? 1 : 0;
    const int64_t cur = me.get("enabled")->i();
    if (cur == nv) return out;
    out.push(writeRec(hexU32(me.get("mgr")) + C::OBJ_MINIEVENTS, 1, h2(static_cast<uint32_t>(cur)),
                      h2(static_cast<uint32_t>(nv)),
                      "minievents " + arg + ": objective manager +4 := " + std::to_string(nv)));
    return out;
  }
  if (prim == "metro_lock") {
    const J mt = metro();
    const uint32_t o = hexU32(mt.get("obj"));
    const int64_t health = mt.get("health")->i(), mx = mt.get("max")->i(), dmg = mt.get("damage")->i();
    if (arg == "on") {
      if (health != mx)
        out.push(writeRec(o + C::METRO_HEALTH, 4, h8(static_cast<uint32_t>(health)), h8(static_cast<uint32_t>(mx)),
                          "metro_lock on: health := max"));
      if (hexU32(mt.get("frac")) != C::F32_ONE)
        out.push(writeRec(o + C::METRO_FRAC, 4, mt.get("frac")->s(), h8(C::F32_ONE), "metro_lock on: fraction := 1.0"));
      if (dmg != 0)
        out.push(writeRec(o + C::METRO_DAMAGE, 1, h2(static_cast<uint32_t>(dmg)), h2(0),
                          "metro_lock on: damage enabled := 0"));
      return out;
    }
    if (cheats && cheats->metroDamage && dmg == 0 && mt.get("lock")->i() == 0)
      out.push(writeRec(o + C::METRO_DAMAGE, 1, h2(0), h2(static_cast<uint32_t>(*cheats->metroDamage)),
                        "metro_lock off: damage enabled := " + std::to_string(*cheats->metroDamage) + " (restored)"));
    return out;
  }
  if (prim == "freecam")
    throw Refusal{"unsupported", "freecam is a guest call (the retail debug camera); kernel executor only"};
  throw Refusal{"unknown_prim", prim};
}

J World::resolve(const std::string& prim, std::optional<int64_t> idx, const std::optional<std::string>& flow,
                 bool strict, const std::optional<std::string>& arg, const CheatState* cheats) {
  try {
    if (isArgPrim(prim)) return resolveCheat(prim, arg.value_or(""), cheats);
    return resolveImpl(prim, idx, flow, strict);
  } catch (const ReadErr& e) {
    throw Refusal{"unreadable", e.msg};
  } catch (const UnknownErr& e) {
    throw Refusal{"unknown", e.msg};
  }
}

J World::resolveImpl(const std::string& prim, std::optional<int64_t> idx, const std::optional<std::string>& flow,
                     bool strict) {
  if (prim == "snapshot" || prim == "pad" || prim == "await_natural") return J::arr();
  if (prim == "play_objective") throw Refusal{"unsupported", "play_objective is a guest call; not in this executor"};
  if (prim == "reach") {
    const auto act = dasVal("objectiveActivated");
    if (!act) throw Refusal{"das_missing", "objectiveActivated"};
    if (strict && *act != 0) throw Refusal{"objective_running", ""};
    std::vector<const J*> c;
    for (const J& t : triggers().a())
      if (idx && fldIs(t, "idx", *idx) && (!flow || sameStr(fld(t, "flow"), *flow))) c.push_back(&t);
    if (c.empty()) throw Refusal{"not_spawned", "no OBJ|TRIGGERS member with objectiveIndex " + std::to_string(idx.value_or(-1))};
    if (c.size() > 1) throw Refusal{"ambiguous", std::to_string(c.size()) + " members"};
    const J& t = *c[0];
    if (!truthy(fld(t, "state")) || !truthy(fld(t, "vt_ok"))) throw Refusal{"state_vt", ""};
    if (strict && (fld(t, "flags")->i() & 1)) throw Refusal{"unavailable", ""};
    if (fld(t, "event")->i() != 0) throw Refusal{"trigger_event", ""};
    const J* lg = fld(t, "logical");
    std::string up = (lg && lg->isStr()) ? lg->s() : "";
    for (char& ch : up)
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (up == "AND" && truthy(fld(t, "power"))) throw Refusal{"and_power", ""};
    const J* ad = fld(t, "attr_distance");
    if (!ad || ad->isNull()) throw Refusal{"distance_attr", ""};
    const J* dist = fld(t, "distance");
    if (!dist || !dist->isStr() || dist->s() != ad->s())
      throw Refusal{"distance_mismatch", (dist ? dist->s() : "") + " vs " + ad->s()};
    const uint32_t stVa = static_cast<uint32_t>(std::stoul(fld(t, "state")->s(), nullptr, 16));
    J out = J::arr();
    out.push(writeRec(stVa + C::STATE_DISTANCE, 4, dist->s(), h8(C::REACH_BITS),
                      "reach " + std::to_string(*idx) + ": trigger state+0x14 := 1e9"));
    return out;
  }
  if (prim == "pass") {
    const auto act = dasVal("objectiveActivated");
    if (!act) throw Refusal{"das_missing", "objectiveActivated"};
    if (strict && *act != 1) throw Refusal{"objective_not_running", ""};
    if (flow) {
      const auto fn = dasStr(base().second, "objectiveWorldFlowName");
      if (fn != flow) throw Refusal{"flow_mismatch", fn.value_or("None") + " running"};
    }
    const auto bi = dasVal("objectiveBitIndex");
    if (!bi) throw Refusal{"das_missing", "objectiveBitIndex"};
    if (!idx || *bi != static_cast<uint32_t>(*idx)) throw Refusal{"index_mismatch", "objectiveBitIndex " + std::to_string(*bi)};
    J bf = baseFsm();
    if (!truthy(bf.get("verified"))) throw Refusal{"no_graph", "base FSM state not verified against the graph"};
    const J* sid = bf.get("state_id");
    if (strict && !(sid->isInt() && sid->i() == C::STATE_IN_OBJECTIVE))
      throw Refusal{"not_in_objective", "base state " + sid->dump()};
    if (!dasSorted()) throw Refusal{"das_unsorted", ""};
    J out = J::arr();
    for (const char* nm : {"objectiveComplete", "objectiveWon"}) {
      const auto d = das(nm);
      if (!d) throw Refusal{"das_missing", nm};
      if (std::get<2>(*d) != C::T_INT)
        throw Refusal{"das_node_type", std::string(nm) + " type " + std::to_string(std::get<2>(*d))};
      out.push(writeRec(std::get<0>(*d) + C::NODE_VALUE, 4, h8(std::get<1>(*d)), h8(1),
                        "pass " + std::to_string(*idx) + ": " + nm + " := 1"));
    }
    return out;
  }
  if (prim == "xp_to_cap") {
    J x = xp();
    if (x.get("cap_off")->isNull()) throw Refusal{"no_cap", ""};
    const uint32_t stat = static_cast<uint32_t>(std::stoul(x.get("stat")->s(), nullptr, 16));
    J out = J::arr();
    out.push(writeRec(stat + static_cast<uint32_t>(x.get("value_off")->i()), 8, x.get("value")->s(),
                      x.get("cap")->s(), "xp_to_cap: CurrentXP := cap"));
    return out;
  }
  throw Refusal{"unknown_prim", prim};
}

J World::resolveRestore(const J& pend) {
  try {
    for (const J& t : triggers().a()) {
      if (sameStr(fld(t, "state"), pend.get("state")->s()) && fldIs(t, "idx", pend.get("idx")->i()) &&
          truthy(fld(t, "vt_ok"))) {
        const std::string d = fld(t, "distance")->s();
        if (d != h8(C::REACH_BITS)) throw Refusal{"restore_value_changed", d};
        const uint32_t st = static_cast<uint32_t>(std::stoul(pend.get("state")->s(), nullptr, 16));
        J out = J::arr();
        out.push(writeRec(st + C::STATE_DISTANCE, 4, d, pend.get("orig")->s(),
                          "reach " + std::to_string(pend.get("idx")->i()) + ": restore state+0x14"));
        return out;
      }
    }
  } catch (const ReadErr& e) {
    throw Refusal{"unreadable", e.msg};
  } catch (const UnknownErr& e) {
    throw Refusal{"unknown", e.msg};
  }
  throw Refusal{"restore_trigger_gone", ""};
}

bool World::restoreDue(const J& pend) {
  try {
    for (const J& t : triggers().a())
      if (sameStr(fld(t, "state"), pend.get("state")->s()) && truthy(fld(t, "engaged"))) return true;
    const auto a = dasVal("objectiveActivated");
    return a && *a == 1;
  } catch (const Refusal&) {
    return false;
  } catch (const ReadErr&) {
    return false;
  } catch (const UnknownErr&) {
    return false;
  }
}

// ============================================================== command language ====
// skipplan.py parse / default_guard / expand_defaults. Whitespace and line breaks are
// Python's str.split() / str.splitlines() sets, so tokens and line numbers agree.
namespace {

bool utf8Decode(const std::string& s, std::u32string& out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else return false;
    if (n > 0 && i + n >= s.size()) return false;
    for (int k = 1; k <= n; k++) {
      const unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    out += cp;
    i += n + 1;
  }
  return true;
}

std::string utf8Encode(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// Python str.isspace() (the str.split() separator set)
bool pySpace(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
         c == 0x3000;
}
// Python str.splitlines() boundaries (\r\n is one)
bool pyLineBreak(char32_t c) {
  return c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E ||
         c == 0x85 || c == 0x2028 || c == 0x2029;
}

std::vector<std::u32string> pyLines(const std::u32string& t) {
  std::vector<std::u32string> lines;
  size_t start = 0, i = 0;
  const size_t n = t.size();
  while (i < n) {
    if (pyLineBreak(t[i])) {
      const size_t end = i;
      if (t[i] == 0x0D && i + 1 < n && t[i + 1] == 0x0A) i++;
      lines.push_back(t.substr(start, end - start));
      start = i + 1;
    }
    i++;
  }
  if (start < n) lines.push_back(t.substr(start));
  return lines;
}

std::vector<std::string> splitU32(const std::u32string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && pySpace(s[i])) i++;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && !pySpace(s[j])) j++;
    out.push_back(utf8Encode(s.substr(i, j - i)));
    i = j;
  }
  return out;
}

bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool reName(const std::string& t) {   // ^[A-Za-z_][A-Za-z0-9_]*$
  if (t.empty() || !(isAlpha(t[0]) || t[0] == '_')) return false;
  for (char c : t)
    if (!(isAlpha(c) || isDigit(c) || c == '_')) return false;
  return true;
}
bool reFlow(const std::string& t) {   // ^[A-Za-z0-9_]+$
  if (t.empty()) return false;
  for (char c : t)
    if (!(isAlpha(c) || isDigit(c) || c == '_')) return false;
  return true;
}
bool reLabel(const std::string& t) {  // ^[A-Za-z0-9_.\-]{1,64}$
  if (t.empty() || t.size() > 64) return false;
  for (char c : t)
    if (!(isAlpha(c) || isDigit(c) || c == '_' || c == '.' || c == '-')) return false;
  return true;
}

const char* const kReserved[] = {"when", "do", "and", "timeout", "deadline", "abort_when"};
bool reserved(const std::string& t) {
  for (const char* r : kReserved)
    if (t == r) return true;
  return false;
}
const char* const kPrims[] = {"reach",    "pass",         "await_natural", "xp_to_cap",  "play_objective", "pad",
                              "snapshot", "pause_timers", "minievents",    "metro_lock", "freecam"};
bool isPrim(const std::string& p) {
  for (const char* r : kPrims)
    if (p == r) return true;
  return false;
}
bool isIdxPrim(const std::string& p) { return p == "reach" || p == "pass" || p == "play_objective"; }
const char* const kPadButtons[] = {"A", "B", "X", "Y", "START", "BACK", "UP", "DOWN", "LEFT", "RIGHT", "LB", "RB", "LS", "RS"};
bool isPadButton(const std::string& b) {
  for (const char* r : kPadButtons)
    if (b == r) return true;
  return false;
}

std::string L(int line) { return "line " + std::to_string(line) + ": "; }

// value as a signed 128-bit-safe comparison helper
struct Val {
  bool neg;
  uint64_t mag;
  bool over;
};

Val pInt(const std::string& tok, const std::string& what, int line) {
  size_t i = 0;
  bool neg = false;
  if (!tok.empty() && tok[0] == '-') { neg = true; i = 1; }
  bool hex = false;
  if (tok.size() >= i + 3 && tok[i] == '0' && (tok[i + 1] == 'x' || tok[i + 1] == 'X')) {
    hex = true;
    for (size_t k = i + 2; k < tok.size(); k++)
      if (!isHex(tok[k])) { hex = false; break; }
  }
  bool dec = tok.size() > i;
  if (dec)
    for (size_t k = i; k < tok.size(); k++)
      if (!isDigit(tok[k])) { dec = false; break; }
  if (!dec && !hex) throw PlanError{L(line) + what + ": expected an integer, got " + tok};
  Val v{neg, 0, false};
  if (dec) {
    for (size_t k = i; k < tok.size(); k++) {
      const uint64_t d = static_cast<uint64_t>(tok[k] - '0');
      if (v.mag > (UINT64_MAX - d) / 10) { v.over = true; break; }
      v.mag = v.mag * 10 + d;
    }
  } else {
    for (size_t k = i + 2; k < tok.size(); k++) {
      const char c = tok[k];
      const uint64_t d = isDigit(c) ? uint64_t(c - '0') : uint64_t((c | 0x20) - 'a' + 10);
      if (v.mag > (UINT64_MAX >> 4)) { v.over = true; break; }
      v.mag = (v.mag << 4) | d;
    }
  }
  if (v.mag == 0 && !v.over) v.neg = false;   // -0 == 0
  return v;
}

// lo <= v < hi for a token value (bounds well inside int64)
bool inRange(const Val& v, int64_t lo, int64_t hi) {
  if (v.over) return false;
  if (v.neg) {
    if (lo >= 0) return false;
    return v.mag <= static_cast<uint64_t>(-lo) && (hi > 0 || static_cast<int64_t>(-static_cast<int64_t>(v.mag)) < hi);
  }
  if (v.mag > static_cast<uint64_t>(INT64_MAX)) return false;
  const int64_t x = static_cast<int64_t>(v.mag);
  return x >= lo && x < hi;
}
int64_t asI64(const Val& v) { return v.neg ? -static_cast<int64_t>(v.mag) : static_cast<int64_t>(v.mag); }

int64_t pBit(const std::string& tok, const std::string& what, int line) {
  const Val v = pInt(tok, what, line);
  if (!inRange(v, 0, C::NUM_BITS)) throw PlanError{L(line) + what + " out of range 0..287"};
  return asI64(v);
}

int64_t pPos(const std::string& tok, const std::string& what, int line) {
  const Val v = pInt(tok, what, line);
  if (v.neg || v.mag == 0) throw PlanError{L(line) + what + " must be > 0"};
  // frida_skip.js pPos: above 2^53-1 is "too large" (skipplan.py accepts any size; no plan
  // needs a timeout or deadline anywhere near it)
  if (v.over || v.mag > 9007199254740991ull) throw PlanError{L(line) + what + " too large"};
  return static_cast<int64_t>(v.mag);
}

Cond mk(const std::string& kind, std::optional<std::string> name = std::nullopt,
        std::optional<int64_t> value = std::nullopt) {
  Cond c;
  c.kind = kind;
  c.name = std::move(name);
  c.value = value;
  return c;
}

Cond parseCond(const std::vector<std::string>& t, int line) {
  if (t.empty()) throw PlanError{L(line) + "empty condition"};
  const std::string& k = t[0];
  if (k == "das") {
    if (t.size() != 4 || t[2] != "==") throw PlanError{L(line) + "expected 'das <name> == <int>'"};
    if (!reName(t[1]) || reserved(t[1])) throw PlanError{L(line) + "bad DAS name"};
    const Val v = pInt(t[3], "das value", line);
    if (!inRange(v, -(int64_t(1) << 31), int64_t(1) << 32)) throw PlanError{L(line) + "das value does not fit 32 bits"};
    return mk("das", t[1], asI64(v));
  }
  if (k == "won") {
    if (t.size() != 2) throw PlanError{L(line) + "expected 'won <bit>'"};
    return mk("won", std::nullopt, pBit(t[1], "bit", line));
  }
  if (k == "flow_loaded") {
    if (t.size() != 2 || !reFlow(t[1]) || reserved(t[1])) throw PlanError{L(line) + "expected 'flow_loaded <flow>'"};
    return mk("flow_loaded", t[1]);
  }
  if (k == "trigger") {
    if (t.size() != 3 || (t[2] != "available" && t[2] != "engaged"))
      throw PlanError{L(line) + "expected 'trigger <idx> available|engaged'"};
    return mk("trigger", t[2], pBit(t[1], "trigger index", line));
  }
  if (k == "stable") {
    if (t.size() != 2) throw PlanError{L(line) + "expected 'stable <ticks>'"};
    return mk("stable", std::nullopt, pPos(t[1], "stable ticks", line));
  }
  if (k == "base_state") {
    if (t.size() != 2) throw PlanError{L(line) + "expected 'base_state <id>'"};
    const Val v = pInt(t[1], "state id", line);
    if (!inRange(v, 0, int64_t(1) << 32)) throw PlanError{L(line) + "state id does not fit u32"};
    return mk("base_state", std::nullopt, asI64(v));
  }
  if (k == "pad_prompt") {
    if (t.size() != 1) throw PlanError{L(line) + "pad_prompt takes no arguments"};
    return mk("pad_prompt");
  }
  if (k == "objective_failed") {
    if (t.size() != 1) throw PlanError{L(line) + "objective_failed takes no arguments"};
    return mk("objective_failed");
  }
  throw PlanError{L(line) + "unknown condition " + k};
}

Step parsePrim(const std::vector<std::string>& t, int line) {
  if (t.empty()) throw PlanError{L(line) + "missing primitive"};
  const std::string& p = t[0];
  const std::vector<std::string> args(t.begin() + 1, t.end());
  if (!isPrim(p)) throw PlanError{L(line) + "unknown primitive " + p};
  Step st;
  st.prim = p;
  st.line = line;
  if (isIdxPrim(p) || p == "await_natural") {
    if (isIdxPrim(p) && args.empty()) throw PlanError{L(line) + p + " needs an objective index"};
    if (args.size() > 2) throw PlanError{L(line) + p + " takes <idx> [<flow>]"};
    if (!args.empty()) st.idx = pBit(args[0], "objective index", line);
    if (args.size() == 2) {
      if (!reFlow(args[1]) || reserved(args[1])) throw PlanError{L(line) + "bad flow name"};
      st.flow = args[1];
    }
  } else if (p == "xp_to_cap") {
    if (!args.empty()) throw PlanError{L(line) + "xp_to_cap takes no arguments"};
  } else if (p == "pad") {
    if (args.empty()) throw PlanError{L(line) + "pad needs at least one button"};
    for (const auto& tok : args) {
      const std::string tt = (!tok.empty() && tok[0] == '@') ? tok.substr(1) : tok;
      bool bad = tt.empty();
      size_t a = 0;
      while (!bad) {
        const size_t plus = tt.find('+', a);
        if (!isPadButton(tt.substr(a, plus == std::string::npos ? std::string::npos : plus - a))) bad = true;
        if (plus == std::string::npos) break;
        a = plus + 1;
      }
      if (bad) throw PlanError{L(line) + "bad pad token " + tok};
    }
    st.buttons = args;
  } else if (p == "snapshot") {
    if (args.size() != 1 || !reLabel(args[0]) || reserved(args[0]))
      throw PlanError{L(line) + "snapshot needs one label [A-Za-z0-9_.-]{1,64}"};
    st.label = args[0];
  } else if (isArgPrim(p)) {
    if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) throw PlanError{L(line) + p + " takes on|off"};
    st.arg = args[0];
  }
  return st;
}

std::vector<Cond> parseConds(const std::vector<std::string>& ctoks, int ln, const std::string& what) {
  if (ctoks.empty()) throw PlanError{L(ln) + what + " with no condition"};
  std::vector<std::vector<std::string>> groups;
  std::vector<std::string> cur;
  for (const auto& t : ctoks) {
    if (t == "and") {
      if (cur.empty()) throw PlanError{L(ln) + "dangling 'and'"};
      groups.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(t);
    }
  }
  if (cur.empty()) throw PlanError{L(ln) + "dangling 'and'"};
  groups.push_back(cur);
  std::vector<Cond> out;
  for (const auto& g : groups) out.push_back(parseCond(g, ln));
  return out;
}

}  // namespace

bool isArgPrim(const std::string& p) {
  return p == "pause_timers" || p == "minievents" || p == "metro_lock" || p == "freecam";
}

std::vector<std::string> pySplit(const std::string& utf8) {
  std::u32string u;
  if (!utf8Decode(utf8, u)) throw PlanError{"not valid UTF-8"};
  return splitU32(u);
}

std::vector<Cond> parseCondTokens(const std::vector<std::string>& toks, int line) {
  return {parseCond(toks, line)};
}

std::vector<Step> parsePlan(const std::string& utf8In) {
  std::u32string text;
  if (!utf8Decode(utf8In, text)) throw PlanError{"the plan is not valid UTF-8"};
  if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
  std::vector<Step> steps;
  std::optional<int64_t> pending, pendingDl;
  std::vector<std::vector<Cond>> aborts;
  std::vector<std::string> labels;
  const auto lines = pyLines(text);
  for (size_t li = 0; li < lines.size(); li++) {
    const int ln = static_cast<int>(li + 1);
    std::u32string code = lines[li];
    const size_t hash = code.find(U'#');
    if (hash != std::u32string::npos) code = code.substr(0, hash);
    size_t a = 0;
    for (;;) {
      const size_t semi = code.find(U';', a);
      const std::u32string stmt = code.substr(a, semi == std::u32string::npos ? std::u32string::npos : semi - a);
      const std::vector<std::string> toks = splitU32(stmt);
      if (!toks.empty()) {
        Step st;
        bool isStep = true;
        if (toks[0] == "timeout") {
          if (toks.size() != 2) throw PlanError{L(ln) + "expected 'timeout <ticks>'"};
          if (pending) throw PlanError{L(ln) + "two timeouts with no step between them"};
          pending = pPos(toks[1], "timeout ticks", ln);
          isStep = false;
        } else if (toks[0] == "deadline") {
          if (toks.size() != 2) throw PlanError{L(ln) + "expected 'deadline <seconds>'"};
          if (pendingDl) throw PlanError{L(ln) + "two deadlines with no step between them"};
          pendingDl = pPos(toks[1], "deadline seconds", ln);
          isStep = false;
        } else if (toks[0] == "abort_when") {
          if (std::find(toks.begin(), toks.end(), "do") != toks.end())
            throw PlanError{L(ln) + "'abort_when' takes conditions only (no 'do')"};
          auto ac = parseConds(std::vector<std::string>(toks.begin() + 1, toks.end()), ln, "'abort_when'");
          for (const auto& c : ac)
            if (c.kind == "stable") throw PlanError{L(ln) + "'stable' is not allowed in 'abort_when'"};
          aborts.push_back(ac);
          isStep = false;
        } else if (toks[0] == "when") {
          const auto d = std::find(toks.begin(), toks.end(), "do");
          if (d == toks.end()) throw PlanError{L(ln) + "'when' without 'do'"};
          auto conds = parseConds(std::vector<std::string>(toks.begin() + 1, d), ln, "'when'");
          int nst = 0;
          for (const auto& c : conds) nst += c.kind == "stable";
          if (nst > 1) throw PlanError{L(ln) + "more than one 'stable' in a guard"};
          st = parsePrim(std::vector<std::string>(d + 1, toks.end()), ln);
          st.when = conds;
        } else if (toks[0] == "do" || toks[0] == "and") {
          throw PlanError{L(ln) + toks[0] + " outside a 'when' guard"};
        } else {
          st = parsePrim(toks, ln);
        }
        if (isStep) {
          st.timeout = pending;
          pending.reset();
          st.deadline = pendingDl;
          pendingDl.reset();
          st.abort = aborts;
          if (st.label) {
            if (std::find(labels.begin(), labels.end(), *st.label) != labels.end())
              throw PlanError{L(ln) + "duplicate snapshot label"};
            labels.push_back(*st.label);
          }
          steps.push_back(std::move(st));
        }
      }
      if (semi == std::u32string::npos) break;
      a = semi + 1;
    }
  }
  if (pending) throw PlanError{"timeout at the end of the plan applies to no step"};
  if (pendingDl) throw PlanError{"deadline at the end of the plan applies to no step"};
  if (!aborts.empty() && (steps.empty() || steps.back().abort.size() != aborts.size()))
    throw PlanError{"abort_when at the end of the plan applies to no step"};
  return steps;
}

std::vector<Cond> defaultGuard(const Step& st) {
  std::vector<Cond> flowC;
  if (st.flow) flowC.push_back(mk("flow_loaded", *st.flow));
  if (st.prim == "reach")
    return {mk("trigger", "available", st.idx), mk("das", "objectiveActivated", 0), mk("das", "triggersAvailable", 1),
            mk("das", "waitingForLoad", 0)};
  if (st.prim == "pass" || st.prim == "xp_to_cap") {
    std::vector<Cond> g{mk("das", "objectiveActivated", 1)};
    g.insert(g.end(), flowC.begin(), flowC.end());
    g.push_back(mk("base_state", std::nullopt, C::STATE_IN_OBJECTIVE));
    g.push_back(mk("das", "waitingForLoad", 0));
    g.push_back(mk("stable", std::nullopt, C::SETTLE_TICKS));
    return g;
  }
  if (st.prim == "await_natural") {
    std::vector<Cond> g{mk("das", "objectiveActivated", 1)};
    g.insert(g.end(), flowC.begin(), flowC.end());
    return g;
  }
  if (st.prim == "play_objective")
    return {mk("trigger", "available", st.idx), mk("das", "triggersAvailable", 1), mk("das", "waitingForLoad", 0),
            mk("das", "objectiveActivated", 0)};
  if (st.prim == "pad") return {mk("pad_prompt")};
  return {};
}

std::vector<Step> expandDefaults(const std::vector<Step>& steps) {
  std::vector<Step> out;
  for (const auto& s : steps) {
    Step c = s;
    if (!c.when) c.when = defaultGuard(c);
    if (!c.timeout && c.prim == "await_natural") c.timeout = C::AWAIT_TIMEOUT;
    out.push_back(std::move(c));
  }
  return out;
}

J condJson(const Cond& c) {
  J j = J::obj();
  j.set("kind", J::str(c.kind));
  if (c.name) j.set("name", J::str(*c.name));
  if (c.value) j.set("value", J::num(*c.value));
  return j;
}

J stepJson(const Step& s) {
  J d = J::obj();
  d.set("prim", J::str(s.prim));
  if (s.idx) d.set("idx", J::num(*s.idx));
  if (s.flow) d.set("flow", J::str(*s.flow));
  if (!s.buttons.empty()) {
    J b = J::arr();
    for (const auto& x : s.buttons) b.push(J::str(x));
    d.set("buttons", std::move(b));
  }
  if (s.label) d.set("label", J::str(*s.label));
  if (s.arg) d.set("arg", J::str(*s.arg));
  if (s.when) {
    J w = J::arr();
    for (const auto& c : *s.when) w.push(condJson(c));
    d.set("when", std::move(w));
  } else {
    d.set("when", J::null());
  }
  d.set("timeout", s.timeout ? J::num(*s.timeout) : J::null());
  if (s.deadline) d.set("deadline", J::num(*s.deadline));
  if (!s.abort.empty()) {
    J ab = J::arr();
    for (const auto& g : s.abort) {
      J gg = J::arr();
      for (const auto& c : g) gg.push(condJson(c));
      ab.push(std::move(gg));
    }
    d.set("abort", std::move(ab));
  }
  return d;
}

std::string condText(const Cond& c) {
  if (c.kind == "das") return "das " + *c.name + " == " + std::to_string(*c.value);
  if (c.kind == "won") return "won " + std::to_string(*c.value);
  if (c.kind == "flow_loaded") return "flow_loaded " + *c.name;
  if (c.kind == "trigger") return "trigger " + std::to_string(*c.value) + " " + *c.name;
  if (c.kind == "stable") return "stable " + std::to_string(*c.value);
  if (c.kind == "base_state") return "base_state " + std::to_string(*c.value);
  if (c.kind == "objective_failed") return "objective_failed";
  return "pad_prompt";
}

// ============================================================== Executor ====
Executor::Executor(const std::vector<Step>& steps, WorldFn world, WriteFn write, bool capsPad, ClockFn clock,
                   EventFn onEvent, std::shared_ptr<CheatState> cheats, CallFn calls)
    : steps_(expandDefaults(steps)),
      world_(std::move(world)),
      write_(std::move(write)),
      capsPad_(capsPad),
      clock_(clock ? std::move(clock) : ClockFn([] { return std::optional<double>(); })),
      onEvent_(std::move(onEvent)),
      cheats_(cheats ? std::move(cheats) : std::make_shared<CheatState>()),
      calls_(std::move(calls)) {
  if (steps_.empty()) status_ = "done";
  newStep();
}

void Executor::newStep() {
  started_.reset();
  startedWall_ = clock_();
  stableN_ = 0;
  stableKey_.reset();
  guardSeen_ = false;
  promptSeen_ = false;
  settleN_ = 0;
  settleKey_.reset();
}

J& Executor::event(const std::string& ev, const std::string& variant, J extra) {
  const Step* st = k_ < steps_.size() ? &steps_[k_] : nullptr;
  J e = J::obj();
  e.set("k", J::num(static_cast<int64_t>(k_)));
  e.set("line", st ? J::num(st->line) : J::null());
  e.set("prim", st ? J::str(st->prim) : J::null());
  e.set("event", J::str(ev));
  e.set("tick", J::num(tickAll_));
  e.set("tick_active", J::num(tickActive_));
  e.set("variant", J::str(variant));
  for (const auto& kv : extra.o()) e.set(kv.first, kv.second);
  events_.push_back(std::move(e));
  if (onEvent_) onEvent_(events_.back());
  return events_.back();
}

void Executor::apply(const J& writes) {
  for (const J& w : writes.a()) write_(w);
}

void Executor::advance() {
  k_ += 1;
  newStep();
  if (k_ >= steps_.size()) status_ = "done";
}

void Executor::stop(const std::string& variant, const std::string& code, const std::string& why) {
  const std::string ev =
      (code == "timeout" || code == "deadline") ? "timeout" : (code == "abort_when" ? "aborted" : "refused");
  J x = J::obj();
  x.set("code", J::str(code));
  x.set("why", J::str(why));
  event(ev, variant, std::move(x));
  status_ = "stopped";
}

bool Executor::deadlineHit(std::optional<double> now) {
  const Step& st = steps_[k_];
  if (!st.deadline || !now) return false;
  if (!startedWall_) {
    startedWall_ = now;
    return false;
  }
  return *now - *startedWall_ >= static_cast<double>(*st.deadline);
}

void Executor::poll(std::optional<double> now) {
  if (status_ != "running") return;
  if (!now) now = clock_();
  if (deadlineHit(now))
    stop("poll", "deadline", "step not done within " + std::to_string(*steps_[k_].deadline) + " s (wall clock)");
}

J Executor::idlePad() {
  J r = J::obj();
  r.set("pressed", J::boolean(false));
  if (status_ != "running") return r;
  const Step st = steps_[k_];
  if (st.prim != "pad") {
    r.set("prim", J::str(st.prim));
    return r;
  }
  World w = world_();
  std::vector<Cond> conds;
  for (const auto& c : *st.when)
    if (c.kind != "stable") conds.push_back(c);
  for (const auto& c : conds)
    if (!w.cond(c)) {
      std::optional<uint32_t> rs;
      try {
        rs = w.runState();
      } catch (...) {
        rs.reset();
      }
      r.set("run_state", rs ? J::num(int64_t(*rs)) : J::null());
      return r;
    }
  if (!capsPad_) {
    stop("idle", "pad_unsupported", "the prompt is up (tick stalled) and this side has no pad injection");
    r.set("refused", J::str("pad_unsupported"));
    return r;
  }
  J x = J::obj();
  J b = J::arr();
  for (const auto& s : st.buttons) b.push(J::str(s));
  x.set("buttons", std::move(b));
  event("pad", "idle", std::move(x));
  advance();
  r.set("pressed", J::boolean(true));
  return r;
}

void Executor::tick(const std::string& variant) {
  tickAll_ += 1;
  if (variant == "active") tickActive_ += 1;
  if (status_ != "running") return;
  World w = world_();
  const Step st = steps_[k_];
  for (const auto& g : st.abort) {
    bool all = true;
    for (const auto& c : g)
      if (!w.cond(c)) { all = false; break; }
    if (all) {
      std::string why;
      for (size_t i = 0; i < g.size(); i++) why += (i ? " and " : "") + condText(g[i]);
      return stop(variant, "abort_when", why);
    }
  }
  if (deadlineHit(clock_()))
    return stop(variant, "deadline", "step not done within " + std::to_string(*st.deadline) + " s (wall clock)");
  if (pendingRestore_ && variant == "active" && w.restoreDue(*pendingRestore_)) {
    J pend = *pendingRestore_;
    pendingRestore_.reset();
    J wr;
    try {
      wr = w.resolveRestore(pend);
    } catch (const Refusal& e) {
      J x = J::obj();
      x.set("code", J::str(e.code));
      x.set("why", J::str(e.why));
      event("restore_skipped", variant, std::move(x));
      return;
    }
    apply(wr);
    J x = J::obj();
    x.set("writes", wr);
    event("restore", variant, std::move(x));
    return;
  }
  if (!started_) started_ = tickActive_;
  const int64_t elapsed = tickActive_ - *started_;
  const bool timedOut = st.timeout && elapsed >= *st.timeout;
  std::vector<Cond> conds;
  std::optional<int64_t> stab;
  for (const auto& c : *st.when) {
    if (c.kind != "stable") conds.push_back(c);
    else if (!stab) stab = c.value;
  }
  if (st.prim == "pad") return tickPad(st, w, conds, variant, timedOut);
  if (st.prim == "reach" && w.runningIndex(*st.idx, st.flow)) {
    J x = J::obj();
    x.set("code", J::str("already_engaged"));
    x.set("why", J::str("objective " + std::to_string(*st.idx) + " is already running (it engaged by itself)"));
    event("skipped", variant, std::move(x));
    return advance();
  }
  if (st.prim == "await_natural" && guardSeen_) {
    if (w.cond(mk("das", "objectiveActivated", 0))) {
      J x = J::obj();
      x.set("writes", J::arr());
      event("executed", variant, std::move(x));
      return advance();
    }
    if (timedOut) {
      if (variant != "active") return;
      J wr;
      try {
        if (!st.idx) throw Refusal{"no_index", "await_natural without an index cannot fall back to pass"};
        wr = w.resolve("pass", st.idx, st.flow);
      } catch (const Refusal& e) {
        return stop(variant, e.code, e.why);
      }
      apply(wr);
      J x = J::obj();
      x.set("writes", wr);
      event("fallback_pass", variant, std::move(x));
      return advance();
    }
    return;
  }
  bool ok = true;
  for (const auto& c : conds)
    if (!w.cond(c)) { ok = false; break; }
  if (stab) {
    if (variant == "active") {
      if (!ok) {
        stableN_ = 0;
        stableKey_.reset();
      } else {
        const J key = w.storyKey();
        if (key.isNull()) {
          stableN_ = 0;
          stableKey_.reset();
        } else {
          const std::string ks = key.dump();
          if (stableKey_ && ks == *stableKey_) {
            stableN_ += 1;
          } else {
            stableN_ = 1;
            stableKey_ = ks;
          }
        }
      }
    }
    ok = ok && variant == "active" && stableN_ >= *stab;
  }
  if (!ok) {
    if (timedOut) stop(variant, "timeout", "guard did not hold within " + std::to_string(*st.timeout) + " ticks");
    return;
  }
  if (st.prim == "await_natural") {
    guardSeen_ = true;
    event("await_start", variant);
    return;
  }
  if (st.prim == "snapshot") {
    J x = J::obj();
    x.set("label", J::str(*st.label));
    event("snapshot", variant, std::move(x));
    return advance();
  }
  if (variant != "active") {
    if (timedOut) stop(variant, "timeout", "no active tick within the timeout");
    return;
  }
  if (st.prim == "freecam" && calls_) {     // a guest-call command: the kernel runs it (CallFn)
    J x;
    try {
      x = calls_(st);
    } catch (const Refusal& e) {
      return stop(variant, e.code, e.why);
    }
    event("executed", variant, std::move(x));
    return advance();
  }
  J wr;
  try {
    wr = w.resolve(st.prim, st.idx, st.flow, true, st.arg, cheats_.get());
  } catch (const Refusal& e) {
    return stop(variant, e.code, e.why);
  }
  apply(wr);
  J x = J::obj();
  x.set("writes", wr);
  event("executed", variant, std::move(x));
  if (isArgPrim(st.prim)) noteCheat(w, st, wr);
  if (st.prim == "reach") {
    const uint32_t va = static_cast<uint32_t>(std::stoul(wr.a()[0].get("va")->s(), nullptr, 16));
    J pend = J::obj();
    pend.set("idx", J::num(*st.idx));
    pend.set("state", J::str(h8(va - 0x14)));
    pend.set("orig", *wr.a()[0].get("old"));
    pendingRestore_ = std::move(pend);
  }
  advance();
}

// skipexec.Executor._note_cheat: what a DT-2 command leaves for its 'off'.
void Executor::noteCheat(World& w, const Step& st, const J& wr) {
  if (st.prim == "pause_timers") {
    if (st.arg && *st.arg == "on") {
      for (const J& x : wr.a()) {
        const uint32_t node = hexU32(x.get("va")) - C::TN_RUN;
        J rec = J::obj();
        rec.set("node", J::str(h8(node)));
        rec.set("owner", J::str(h8(w.mem().u32(node + C::TN_OWNER))));
        rec.set("hash", J::str(h8(w.mem().u32(node + C::TN_HASH))));
        cheats_->timers.push(std::move(rec));
      }
    } else {
      cheats_->timers = J::arr();
    }
  } else if (st.prim == "metro_lock") {
    if (st.arg && *st.arg == "on") {
      for (const J& x : wr.a())
        if (x.get("width")->i() == 1) cheats_->metroDamage = static_cast<int64_t>(hexU32(x.get("old")));
    } else {
      cheats_->metroDamage.reset();
    }
  }
}

void Executor::tickPad(const Step& st, World& w, const std::vector<Cond>& conds, const std::string& variant,
                       bool timedOut) {
  bool hasPrompt = false;
  for (const auto& c : conds) hasPrompt = hasPrompt || c.kind == "pad_prompt";
  if (!capsPad_ && !hasPrompt) return stop(variant, "pad_unsupported", "this side has no pad injection");
  if (hasPrompt && w.cond(mk("pad_prompt"))) promptSeen_ = true;
  bool all = true;
  for (const auto& c : conds)
    if (!w.cond(c)) { all = false; break; }
  if (all) {
    if (!capsPad_) return stop(variant, "pad_unsupported", "the prompt is up and this side has no pad injection");
    J x = J::obj();
    J b = J::arr();
    for (const auto& s : st.buttons) b.push(J::str(s));
    x.set("buttons", std::move(b));
    event("pad", variant, std::move(x));
    return advance();
  }
  if (hasPrompt && !promptSeen_ && variant == "active") {
    bool others = true;
    for (const auto& c : conds) {
      if (c.kind == "pad_prompt") continue;
      if (!w.cond(c)) { others = false; break; }
    }
    J key = (others && w.pastPromptPoint()) ? w.storyKey() : J::null();
    if (key.isNull()) {
      settleN_ = 0;
      settleKey_.reset();
    } else {
      const std::string ks = key.dump();
      if (settleN_ && settleKey_ && ks == *settleKey_) {
        settleN_ += 1;
      } else {
        settleN_ = 1;
        settleKey_ = ks;
      }
    }
    if (settleN_ >= C::SETTLE_TICKS) {
      J x = J::obj();
      x.set("code", J::str("no_prompt"));
      x.set("why", J::str("settled past the prompt point without run state 4 (no popup)"));
      event("skipped", variant, std::move(x));
      return advance();
    }
  }
  if (timedOut) stop(variant, "timeout", "pad guard did not hold");
}

// ============================================================== report ====
const char* const kReportConds[] = {
    "das objectiveActivated == 1", "das objectiveActivated == 0", "das triggersAvailable == 1",
    "das waitingForLoad == 0", "das objectiveWorldFlowName == 0", "won 147", "won 0", "won 1", "won 287",
    "flow_loaded MSTobj01", "flow_loaded msTOBJ01", "flow_loaded BaseWorldFlow", "flow_loaded WARobj01",
    "trigger 0 engaged", "trigger 0 available", "trigger 127 available", "trigger 127 engaged",
    "trigger 144 available", "base_state 107", "base_state 1793378647", "pad_prompt",
    "das noSuchNodeForTheDifferential == 0", "objective_failed", "das objectiveBitIndex == 0",
    "das miniEventEngaged == 0", nullptr};

static const char* const kReportDas[] = {
    "objectiveActivated", "objectiveComplete", "objectiveWon", "objectiveBitIndex", "objectiveWorldFlowName",
    "triggersAvailable", "waitingForLoad", "triggerEngaged", "currentHeroPointChapter", "GoToNextChapter",
    "numObjectives", "MaxMetropolisHealth", nullptr};

static J refusedJ(const std::string& code) {
  J r = J::obj();
  r.set("refused", J::str(code));
  return r;
}

template <class F>
static void grab(J& out, const char* key, F fn) {
  try {
    out.set(key, fn());
  } catch (const Refusal& e) {
    out.set(key, refusedJ(e.code));
  } catch (const ReadErr&) {
    out.set(key, refusedJ("unreadable"));
  } catch (const UnknownErr&) {
    out.set(key, refusedJ("unknown"));
  }
}

static J intList(const std::vector<int64_t>& v) {
  J a = J::arr();
  for (int64_t x : v) a.push(J::num(x));
  return a;
}

J report(World& w, const std::vector<StepKey>& keys, const std::vector<std::string>& condTexts) {
  J out = J::obj();
  grab(out, "base", [&] {
    const auto b = w.base();
    J r = J::obj();
    r.set("flow", J::str(h8(b.first)));
    r.set("das", J::str(h8(b.second)));
    return r;
  });
  std::vector<std::string> names;
  for (int i = 0; kReportDas[i]; i++) names.push_back(kReportDas[i]);
  for (int i = 0; i < 9; i++) names.push_back("objectiveStatus" + std::to_string(i));
  for (int i = 0; i < 9; i++) names.push_back("objectiveFound" + std::to_string(i));
  names.push_back("noSuchNodeForTheDifferential");
  J das = J::obj();
  for (const auto& nm : names) {
    try {
      const auto d = w.das(nm);
      if (!d) {
        das.set(nm, J::null());
      } else {
        J r = J::obj();
        r.set("node", J::str(h8(std::get<0>(*d))));
        r.set("value", J::str(h8(std::get<1>(*d))));
        r.set("type", J::num(std::get<2>(*d)));
        das.set(nm, std::move(r));
      }
    } catch (const Refusal& e) {
      das.set(nm, refusedJ(e.code));
    } catch (const ReadErr&) {
      das.set(nm, refusedJ("unreadable"));
    } catch (const UnknownErr&) {
      das.set(nm, refusedJ("unknown"));
    }
  }
  out.set("das", std::move(das));
  grab(out, "won", [&] { return intList(w.wonBits()); });
  grab(out, "found", [&] { return intList(w.wonBits("objectiveFound")); });
  grab(out, "loaded", [&] {
    J a = J::arr();
    for (const auto& s : w.loaded()) a.push(J::str(s));
    return a;
  });
  grab(out, "triggers", [&] { return w.triggers(); });
  grab(out, "base_fsm", [&] { return w.baseFsm(); });
  grab(out, "xp", [&] { return w.xp(); });
  grab(out, "run_state", [&] { return optU32(w.runState()); });
  grab(out, "das_sorted", [&] { return J::boolean(w.dasSorted()); });
  out.set("story_key", w.storyKey());
  J conds = J::obj();
  for (const auto& t : condTexts) conds.set(t, J::boolean(w.cond(parseCondTokens(pySplit(t), 0)[0])));
  out.set("conds", std::move(conds));
  out.set("cheats", w.cheatsReport());
  J wr = J::obj();
  for (const auto& sk : keys) {
    const std::string key = sk.prim + " " + (sk.idx ? std::to_string(*sk.idx) : "None") + " " +
                            (sk.flow ? *sk.flow : std::string("None")) + (sk.arg ? " " + *sk.arg : std::string());
    for (bool strict : {true, false}) {
      const std::string k2 = strict ? key : "probe " + key;
      try {
        J r = J::obj();
        r.set("writes", w.resolve(sk.prim, sk.idx, sk.flow, strict, sk.arg));
        wr.set(k2, std::move(r));
      } catch (const Refusal& e) {
        wr.set(k2, refusedJ(e.code));
      }
    }
  }
  out.set("writes", std::move(wr));
  return out;
}

}  // namespace skip
}  // namespace devtools
}  // namespace sr
