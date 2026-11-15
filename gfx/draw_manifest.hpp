// ── iter-457 gate 3: THE STRICT DRAW-SUPPRESSION MANIFEST ───────────────────
//
// The director's selector contract (CLAIMS_LEDGER, 2026-09-01):
//
//   "The suppressor must key on an explicit set of zero-based `hleSubmitDraw`
//    ordinals from the pinned replay, not directly on shader pairs or a guessed
//    semantic family. Use a strict manifest file rather than an environment
//    string large enough to hit command-line limits. The manifest must pin the
//    trace SHA-256 and expected admitted-draw count, contain sorted
//    non-overlapping ordinals/ranges, and be rejected before treatment if it is
//    empty, malformed, overlapping, out of range, or names the wrong trace."
//
// WHY A HEADER OF PURE FUNCTIONS: the same reason vb_publish.hpp is one. Every
// rejection rule below is a claim that has to be PROVEN, and a rule buried in
// the backend can only be tested by rebuilding and replaying. Here the parser,
// the overlap check and the selection predicate are ordinary functions a unit
// test drives directly with adversarial fixtures — which is what H-S1 demands.
//
// ⚠ THIS IS AN OMISSION INSTRUMENT, NOT FORCING. It removes draws WE would have
// submitted, to measure what they contributed. No guest memory is written, no
// guest decision is overridden, nothing is fabricated. It is default-OFF and
// requires an explicit manifest path.
//
// MANIFEST GRAMMAR (line-oriented, '#' comments, blank lines ignored):
//
//     trace-sha256 <64 lowercase hex>     required, exactly once
//     admitted-draws <N>                  required, exactly once
//     select all                          | select <ordinal> | select <lo>-<hi>
//
// `select all` and explicit selectors are mutually exclusive. Explicit ranges
// are INCLUSIVE, must be strictly ascending, must not touch or overlap, and
// every ordinal must be < admitted-draws.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sr::gfx::manifest {

// Rejection reasons. Named rather than boolean so a refusal says WHY — the
// difference between a fail-closed instrument and one that silently selects
// nothing and reports a clean run.
enum class Reject : uint32_t {
  None = 0,
  Unreadable,        // file missing or unopenable
  MalformedLine,     // a line matched no production
  MissingSha,        // no trace-sha256 directive
  MissingAdmitted,   // no admitted-draws directive
  DuplicateDirective,// trace-sha256 or admitted-draws given twice
  BadSha,            // not 64 lowercase hex characters
  BadAdmitted,       // zero or unparsable
  EmptySelection,    // parsed cleanly but selects nothing
  MixedAll,          // `select all` combined with explicit selectors
  BadRange,          // lo > hi
  Unsorted,          // ranges not strictly ascending
  Overlap,           // ranges overlap or abut ambiguously
  OutOfRange,        // an ordinal >= admittedDraws
  TraceMismatch,     // pinned sha does not match the trace being replayed
  AdmittedMismatch,  // observed admitted count != pinned admittedDraws
};

inline const char* rejectName(Reject r) {
  switch (r) {
    case Reject::None:              return "none";
    case Reject::Unreadable:        return "unreadable";
    case Reject::MalformedLine:     return "malformed-line";
    case Reject::MissingSha:        return "missing-trace-sha256";
    case Reject::MissingAdmitted:   return "missing-admitted-draws";
    case Reject::DuplicateDirective:return "duplicate-directive";
    case Reject::BadSha:            return "bad-sha";
    case Reject::BadAdmitted:       return "bad-admitted-draws";
    case Reject::EmptySelection:    return "empty-selection";
    case Reject::MixedAll:          return "mixed-all-and-explicit";
    case Reject::BadRange:          return "bad-range";
    case Reject::Unsorted:          return "unsorted";
    case Reject::Overlap:           return "overlap";
    case Reject::OutOfRange:        return "out-of-range";
    case Reject::TraceMismatch:     return "trace-mismatch";
    case Reject::AdmittedMismatch:  return "admitted-count-mismatch";
  }
  return "unknown";
}


// ── SHA-256, so "names the wrong trace" is a RULE and not a note ────────────
// The contract requires a manifest to be "rejected before treatment if it is
// empty, malformed, overlapping, out of range, or NAMES THE WRONG TRACE". The
// first four are pure syntax; the last needs the trace's actual digest, and the
// codebase had no SHA-256. This is the reference implementation (FIPS 180-4),
// kept here rather than pulled in as a dependency because it is used in exactly
// one place and is covered by a known-answer test in the R20 suite.
namespace detail {

struct Sha256 {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint64_t len = 0;
  uint8_t  buf[64] = {0};
  size_t   have = 0;

  static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
      0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
      0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
      0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
      0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
      0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
      0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
      0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
      0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
             ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
      const uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
      const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + mj;
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }

  void update(const uint8_t* p, size_t n) {
    len += (uint64_t)n * 8u;
    while (n) {
      const size_t take = (64 - have) < n ? (64 - have) : n;
      std::memcpy(buf + have, p, take);
      have += take; p += take; n -= take;
      if (have == 64) { block(buf); have = 0; }
    }
  }

  std::string hex() {
    uint64_t bitlen = len;
    uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t z = 0;
    while (have != 56) update(&z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bitlen >> (56 - 8*i));
    len -= 64;                      // the length block is not message data
    update(lb, 8);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i)
      for (int b = 3; b >= 0; --b) {
        const uint8_t v = (uint8_t)(h[i] >> (8*b));
        out.push_back(hexd[v >> 4]); out.push_back(hexd[v & 15]);
      }
    return out;
  }
};

}  // namespace detail

// Lowercase hex SHA-256 of a string. Empty string on nothing is still a valid
// digest, so callers distinguish "no file" by the file helper below.
inline std::string sha256Bytes(const std::string& data) {
  detail::Sha256 s;
  s.update((const uint8_t*)data.data(), data.size());
  return s.hex();
}

// Streams the file so a 288 MB trace is not slurped into memory. Returns an
// EMPTY string when the file cannot be read, which the caller must treat as a
// failure to verify rather than as a match.
inline std::string sha256File(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return std::string();
  detail::Sha256 s;
  std::vector<uint8_t> buf(1 << 20);
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
    s.update(buf.data(), n);
  std::fclose(f);
  return s.hex();
}

struct Range {            // INCLUSIVE
  uint64_t lo = 0, hi = 0;
};

struct Manifest {
  std::string traceSha;          // 64 lowercase hex, pinned
  uint64_t    admittedDraws = 0; // expected admitted-draw total of the fixture
  bool        selectAll = false;
  std::vector<Range> ranges;     // sorted, non-overlapping; empty iff selectAll
  uint64_t    requested = 0;     // total ordinals selected

  // THE SELECTION PREDICATE. Binary search: the manifest may name thousands of
  // ordinals and this runs per submitted draw.
  bool selects(uint64_t ord) const {
    if (selectAll) return ord < admittedDraws;
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (ord < ranges[mid].lo)      hi = mid;
      else if (ord > ranges[mid].hi) lo = mid + 1;
      else                           return true;
    }
    return false;
  }
};

// ── the parser ──────────────────────────────────────────────────────────────
// Takes the file's TEXT, so a unit test never needs a temp file. Returns
// Reject::None and fills `out` on success; on any failure `out` is left in a
// state that selects NOTHING, so a caller that ignores the return value still
// suppresses zero draws rather than an arbitrary set.
inline Reject parse(const std::string& text, Manifest* out) {
  *out = Manifest{};
  bool haveSha = false, haveAdmitted = false;
  std::vector<Range> ranges;
  bool sawAll = false, sawExplicit = false;

  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;

    // strip comment and surrounding whitespace
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    size_t b = line.find_first_not_of(" \t\r");
    if (b == std::string::npos) { if (nl >= text.size()) break; continue; }
    const size_t e = line.find_last_not_of(" \t\r");
    line = line.substr(b, e - b + 1);
    if (line.empty()) { if (nl >= text.size()) break; continue; }

    char kw[32] = {0}, rest[256] = {0};
    if (std::sscanf(line.c_str(), "%31s %255[^\n]", kw, rest) != 2)
      return Reject::MalformedLine;

    if (std::strcmp(kw, "trace-sha256") == 0) {
      if (haveSha) return Reject::DuplicateDirective;
      std::string sha(rest);
      while (!sha.empty() && (sha.back() == ' ' || sha.back() == '\t')) sha.pop_back();
      if (sha.size() != 64) return Reject::BadSha;
      for (char c : sha)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return Reject::BadSha;
      out->traceSha = sha;
      haveSha = true;
    } else if (std::strcmp(kw, "admitted-draws") == 0) {
      if (haveAdmitted) return Reject::DuplicateDirective;
      char* endp = nullptr;
      const unsigned long long v = std::strtoull(rest, &endp, 10);
      if (endp == rest || v == 0) return Reject::BadAdmitted;
      while (endp && *endp == ' ') ++endp;
      if (endp && *endp != '\0' && *endp != '\r') return Reject::BadAdmitted;
      out->admittedDraws = (uint64_t)v;
      haveAdmitted = true;
    } else if (std::strcmp(kw, "select") == 0) {
      std::string arg(rest);
      while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t' ||
                              arg.back() == '\r')) arg.pop_back();
      if (arg == "all") { sawAll = true; continue; }
      sawExplicit = true;
      unsigned long long lo = 0, hi = 0;
      const size_t dash = arg.find('-');
      char* endp = nullptr;
      if (dash == std::string::npos) {
        lo = std::strtoull(arg.c_str(), &endp, 10);
        if (endp == arg.c_str() || (endp && *endp != '\0')) return Reject::MalformedLine;
        hi = lo;
      } else {
        const std::string a = arg.substr(0, dash), c = arg.substr(dash + 1);
        if (a.empty() || c.empty()) return Reject::MalformedLine;
        lo = std::strtoull(a.c_str(), &endp, 10);
        if (endp == a.c_str() || (endp && *endp != '\0')) return Reject::MalformedLine;
        hi = std::strtoull(c.c_str(), &endp, 10);
        if (endp == c.c_str() || (endp && *endp != '\0')) return Reject::MalformedLine;
      }
      if (lo > hi) return Reject::BadRange;
      ranges.push_back(Range{(uint64_t)lo, (uint64_t)hi});
    } else {
      return Reject::MalformedLine;
    }
    if (nl >= text.size()) break;
  }

  if (!haveSha)      return Reject::MissingSha;
  if (!haveAdmitted) return Reject::MissingAdmitted;
  if (sawAll && sawExplicit) return Reject::MixedAll;

  if (sawAll) {
    out->selectAll = true;
    out->requested = out->admittedDraws;
    return Reject::None;
  }
  if (ranges.empty()) return Reject::EmptySelection;

  // Strictly ascending and non-touching, checked in AUTHOR order rather than by
  // sorting first: a manifest that only becomes valid after we reorder it is a
  // manifest whose author did not mean what they wrote.
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (ranges[i].hi >= out->admittedDraws) return Reject::OutOfRange;
    if (i == 0) continue;
    if (ranges[i].lo <= ranges[i - 1].hi)  return Reject::Overlap;
    if (ranges[i].lo < ranges[i - 1].lo)   return Reject::Unsorted;
  }

  uint64_t total = 0;
  for (const Range& r : ranges) total += (r.hi - r.lo + 1);
  if (total == 0) return Reject::EmptySelection;

  out->ranges    = std::move(ranges);
  out->requested = total;
  return Reject::None;
}

inline Reject parseFile(const char* path, Manifest* out) {
  *out = Manifest{};
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return Reject::Unreadable;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  std::fclose(f);
  return parse(text, out);
}

// ── the completion check ────────────────────────────────────────────────────
// "At completion print and check `requested`, `matched`, `suppressed`, and total
// submitted; any inequality invalidates the replay and must return nonzero."
struct Tally {
  uint64_t requested = 0, matched = 0, suppressed = 0, totalSubmitted = 0;
};

inline bool tallyConsistent(const Tally& t) {
  return t.requested == t.matched && t.matched == t.suppressed &&
         t.suppressed <= t.totalSubmitted;
}

}  // namespace sr::gfx::manifest
