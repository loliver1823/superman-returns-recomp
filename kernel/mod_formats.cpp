// iter-464 modcore: JSON, SHA-256, FNV-1a-64, RefPack header, SRPATCH1 (mod_formats.hpp).
#include "mod_formats.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace sr {
namespace kernel {

// ================================================================ JSON
const JsonValue* JsonValue::get(const std::string& key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

namespace {

struct JsonParser {
  const std::string& s;
  size_t p = 0;
  std::string err;
  int depth = 0;

  explicit JsonParser(const std::string& t) : s(t) {}

  bool fail(const char* what) {
    if (err.empty()) {
      size_t line = 1, col = 1;
      for (size_t i = 0; i < p && i < s.size(); ++i) {
        if (s[i] == '\n') { ++line; col = 1; } else ++col;
      }
      char b[160];
      std::snprintf(b, sizeof b, "line %zu col %zu: %s", line, col, what);
      err = b;
    }
    return false;
  }
  void ws() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
  }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s.compare(p, n, w) != 0) return false;
    p += n;
    return true;
  }
  static void putUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t* v) {
    if (p + 4 > s.size()) return fail("truncated \\u escape");
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s[p++];
      r <<= 4;
      if (c >= '0' && c <= '9') r |= c - '0';
      else if (c >= 'a' && c <= 'f') r |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') r |= c - 'A' + 10;
      else return fail("bad \\u escape");
    }
    *v = r;
    return true;
  }
  bool str(std::string* o) {
    if (p >= s.size() || s[p] != '"') return fail("expected a string");
    ++p;
    while (true) {
      if (p >= s.size()) return fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(s[p++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("control character in a string");
      if (c != '\\') { *o += static_cast<char>(c); continue; }
      if (p >= s.size()) return fail("unterminated escape");
      const char e = s[p++];
      switch (e) {
        case '"': *o += '"'; break;
        case '\\': *o += '\\'; break;
        case '/': *o += '/'; break;
        case 'b': *o += '\b'; break;
        case 'f': *o += '\f'; break;
        case 'n': *o += '\n'; break;
        case 'r': *o += '\r'; break;
        case 't': *o += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = 0;
            if (!lit("\\u") || !hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("lone low surrogate");
          }
          putUtf8(*o, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
  }
  bool num(JsonValue* v) {
    const size_t st = p;
    if (p < s.size() && s[p] == '-') ++p;
    if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
    if (s[p] == '0') ++p;
    else while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    bool integral = true;
    if (p < s.size() && s[p] == '.') {
      integral = false;
      ++p;
      if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      integral = false;
      ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
      if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    const std::string t = s.substr(st, p - st);
    v->type = JsonValue::Type::Number;
    v->num = std::strtod(t.c_str(), nullptr);
    if (integral && t.size() <= 18) {
      v->isInt = true;
      v->i64 = std::strtoll(t.c_str(), nullptr, 10);
    }
    return true;
  }
  bool value(JsonValue* v) {
    if (++depth > 64) return fail("nesting deeper than 64");
    ws();
    if (p >= s.size()) return fail("unexpected end");
    const char c = s[p];
    bool ok = true;
    if (c == '{') {
      ++p;
      v->type = JsonValue::Type::Object;
      ws();
      if (p < s.size() && s[p] == '}') { ++p; --depth; return true; }
      while (true) {
        ws();
        std::string k;
        if (!str(&k)) return false;
        ws();
        if (p >= s.size() || s[p] != ':') return fail("expected ':'");
        ++p;
        JsonValue m;
        if (!value(&m)) return false;
        v->obj.emplace_back(std::move(k), std::move(m));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == '}') { ++p; break; }
        return fail("expected ',' or '}'");
      }
    } else if (c == '[') {
      ++p;
      v->type = JsonValue::Type::Array;
      ws();
      if (p < s.size() && s[p] == ']') { ++p; --depth; return true; }
      while (true) {
        JsonValue m;
        if (!value(&m)) return false;
        v->arr.push_back(std::move(m));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == ']') { ++p; break; }
        return fail("expected ',' or ']'");
      }
    } else if (c == '"') {
      v->type = JsonValue::Type::String;
      ok = str(&v->str);
    } else if (lit("true")) {
      v->type = JsonValue::Type::Bool; v->b = true;
    } else if (lit("false")) {
      v->type = JsonValue::Type::Bool; v->b = false;
    } else if (lit("null")) {
      v->type = JsonValue::Type::Null;
    } else {
      ok = num(v);
    }
    --depth;
    return ok;
  }
};

}  // namespace

bool jsonParse(const std::string& text, JsonValue* out, std::string* err) {
  std::string t = text;
  if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
      static_cast<unsigned char>(t[2]) == 0xBF)
    t = t.substr(3);
  JsonParser ps(t);
  JsonValue v;
  if (!ps.value(&v)) {
    if (err) *err = ps.err;
    return false;
  }
  ps.ws();
  if (ps.p != t.size()) {
    ps.fail("trailing characters");
    if (err) *err = ps.err;
    return false;
  }
  *out = std::move(v);
  return true;
}

std::string jsonQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

// ================================================================ SHA-256 (FIPS 180-4)
namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  len_ += n;
  if (blen_) {
    const size_t t = (n < 64 - blen_) ? n : 64 - blen_;
    std::memcpy(buf_ + blen_, p, t);
    blen_ += t; p += t; n -= t;
    if (blen_ == 64) { block(buf_); blen_ = 0; }
  }
  while (n >= 64) { block(p); p += 64; n -= 64; }
  if (n) { std::memcpy(buf_, p, n); blen_ = n; }
}

void Sha256::final(uint8_t out[32]) {
  const uint64_t bits = len_ * 8;
  const uint8_t pad = 0x80;
  update(&pad, 1);
  const uint8_t z = 0;
  while (blen_ != 56) update(&z, 1);
  uint8_t L[8];
  for (int i = 0; i < 8; ++i) L[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(L, 8);
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<uint8_t>(h_[i] >> 24); out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
    out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8); out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
  }
}

std::string hexOf(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
  return o;
}

std::string sha256Hex(const void* data, size_t n) {
  Sha256 h;
  h.update(data, n);
  uint8_t d[32];
  h.final(d);
  return hexOf(d, 32);
}

uint64_t fnv1a64Upper(const std::string& name) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : name) {
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
    h = (h ^ c) * 0x100000001B3ull;
  }
  return h;
}

bool refpackDecompressedSize(const uint8_t* head, size_t n, uint64_t* size) {
  if (n < 5 || head[1] != 0xFB || (head[0] & 0x3E) != 0x10) return false;
  const size_t w = (head[0] & 0x80) ? 4 : 3;
  const size_t p = 2 + ((head[0] & 1) ? w : 0);
  if (n < p + w) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < w; ++i) v = (v << 8) | head[p + i];
  *size = v;
  return true;
}

// ================================================================ SRPATCH1
namespace {
bool readU(const std::vector<uint8_t>& b, size_t* p, uint64_t* v) {
  uint64_t n = 0;
  int shift = 0;
  for (int k = 0; k < 10; ++k) {
    if (*p >= b.size()) return false;
    const uint8_t c = b[(*p)++];
    if (shift == 63 && (c & 0x7E)) return false;  // overflows 64 bits
    n |= uint64_t(c & 0x7F) << shift;
    shift += 7;
    if (!(c & 0x80)) { *v = n; return true; }
  }
  return false;
}
bool readS(const std::vector<uint8_t>& b, size_t* p, int64_t* v) {
  uint64_t z = 0;
  if (!readU(b, p, &z)) return false;
  *v = (z & 1) ? -static_cast<int64_t>((z + 1) >> 1) : static_cast<int64_t>(z >> 1);
  return true;
}
uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}  // namespace

bool srpatchParse(const std::vector<uint8_t>& patch, SrPatch* out, std::string* err) {
  auto bad = [&](const char* m) { if (err) *err = m; return false; };
  if (patch.size() < 0x61) return bad("patch shorter than its header");
  if (std::memcmp(patch.data(), "SRPATCH1", 8) != 0) return bad("not an SRPATCH1 file");
  if (le32(&patch[8]) != 1 || le32(&patch[12]) != 0) return bad("unsupported SRPATCH format");
  SrPatch r;
  r.baseSize = le64(&patch[0x10]);
  std::memcpy(r.baseSha, &patch[0x18], 32);
  r.outSize = le64(&patch[0x38]);
  std::memcpy(r.outSha, &patch[0x40], 32);
  size_t p = 0x60;
  uint64_t cursor = 0, fcursor = 0, produced = 0;
  while (true) {
    if (p >= patch.size()) return bad("missing END");
    const uint8_t k = patch[p++];
    if (k == kSrpEnd) {
      if (p != patch.size()) return bad("bytes after END");
      break;
    }
    SrPatch::Op op{k, 0, 0, 0};
    if (k == kSrpCopy || k == kSrpCopyF) {
      int64_t d = 0;
      uint64_t L = 0;
      if (!readS(patch, &p, &d) || !readU(patch, &p, &L)) return bad("truncated copy");
      const int64_t off = static_cast<int64_t>(k == kSrpCopy ? cursor : fcursor) + d;
      if (L == 0 || off < 0) return bad("empty or negative copy");
      op.off = static_cast<uint64_t>(off);
      op.len = L;
      if (k == kSrpCopy) {
        if (op.off + L > r.baseSize || op.off + L < op.off) return bad("COPY outside the base");
        cursor = op.off + L;
      } else {
        fcursor = op.off + L;
        r.usesFile = true;
      }
    } else if (k == kSrpAdd) {
      uint64_t L = 0;
      if (!readU(patch, &p, &L) || L == 0 || L > patch.size() - p) return bad("ADD runs past the patch end");
      op.len = L;
      op.addAt = p;
      p += static_cast<size_t>(L);
    } else if (k == kSrpFill) {
      if (p >= patch.size()) return bad("truncated FILL");
      op.off = patch[p++];
      uint64_t L = 0;
      if (!readU(patch, &p, &L) || L == 0) return bad("empty FILL");
      op.len = L;
    } else {
      return bad("unknown SRPATCH op");
    }
    produced += op.len;
    if (produced > r.outSize) return bad("ops produce more than the output size");
    r.ops.push_back(op);
  }
  if (produced != r.outSize) return bad("ops produce a different size than the header says");
  *out = std::move(r);
  return true;
}

bool srpatchBaseMatches(const SrPatch& parsed, const std::vector<uint8_t>& base) {
  if (base.size() != parsed.baseSize) return false;
  Sha256 h;
  h.update(base.data(), base.size());
  uint8_t d[32];
  h.final(d);
  return std::memcmp(d, parsed.baseSha, 32) == 0;
}

bool srpatchApply(const std::vector<uint8_t>& patch, const SrPatch& parsed, const std::vector<uint8_t>& base,
                  const SrFileReader& file, std::vector<uint8_t>* out, std::string* err) {
  auto bad = [&](const char* m) { if (err) *err = m; return false; };
  if (!srpatchBaseMatches(parsed, base)) return bad("the base is not the one this patch was made for");
  if (parsed.usesFile && !file) return bad("the patch copies from the base file, which is not available");
  std::vector<uint8_t> o;
  o.resize(static_cast<size_t>(parsed.outSize));
  size_t w = 0;
  for (const auto& op : parsed.ops) {
    switch (op.kind) {
      case kSrpCopy: std::memcpy(&o[w], &base[static_cast<size_t>(op.off)], static_cast<size_t>(op.len)); break;
      case kSrpCopyF:
        if (!file(op.off, op.len, &o[w])) return bad("COPYF outside the base file");
        break;
      case kSrpAdd: std::memcpy(&o[w], &patch[op.addAt], static_cast<size_t>(op.len)); break;
      case kSrpFill: std::memset(&o[w], static_cast<int>(op.off), static_cast<size_t>(op.len)); break;
      default: return bad("bad op");
    }
    w += static_cast<size_t>(op.len);
  }
  Sha256 h;
  h.update(o.data(), o.size());
  uint8_t d[32];
  h.final(d);
  if (std::memcmp(d, parsed.outSha, 32) != 0) return bad("the output does not match the patch's output hash");
  *out = std::move(o);
  return true;
}

}  // namespace kernel
}  // namespace sr
