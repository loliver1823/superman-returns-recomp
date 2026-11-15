// Lane E (RELEASE_PLAN step 5, E3-E6 r3 / r4): difftest --asset-receipt-test - the [ASSET] MAIN receipt's matcher and
// its log lines (kernel/asset_receipt.hpp). Hermetic: synthetic .apt / .const parts and a synthetic guest heap.
#include "../../kernel/asset_receipt.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ar = sr::kernel::assetreceipt;

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    return;
  }
  ++g_fail;
  std::printf("  FAIL: %s\n", what);
}

std::vector<uint8_t> part(const char* magic, uint8_t fill, size_t n) {
  std::vector<uint8_t> v(n, fill);
  std::memcpy(v.data(), magic, std::strlen(magic));
  for (size_t i = std::strlen(magic); i < n; ++i) v[i] = uint8_t(fill + i * 7);
  return v;
}

ar::Candidate cand(const char* source, uint8_t fill) {
  ar::Candidate c;
  c.movie = "MAIN";
  c.source = source;
  c.file = std::string("file-") + source;
  c.sha256 = std::string(64, source[0]);
  c.apt = part("Apt Data:1:7:4", fill, 300);
  c.cst = part("Apt constant file\x1a", uint8_t(fill + 1), 200);
  return c;
}

// A minimal BGFA1.05 container (kernel/costume_viewer_model.cpp bgfaHead / bgfaRec): a 0x40-byte header, a TOC of
// 10-byte records {lead, id:1, off:4, size:4} at 0x40, the payloads after it.
std::vector<uint8_t> bgfa(const std::vector<std::vector<uint8_t>>& parts) {
  auto put32 = [](std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
  };
  const size_t rec = 10, toc = 0x40;
  std::vector<uint8_t> b(toc + rec * parts.size(), 0);
  std::memcpy(b.data(), "BGFA1.05", 8);
  put32(b, 0x0C, uint32_t(parts.size()));
  put32(b, 0x10, uint32_t(toc));
  b[0x21] = 0;   // wA
  b[0x22] = 1;   // wId
  b[0x23] = 4;   // wOff
  b[0x24] = 4;   // wSize
  b[0x25] = 0;   // wUnc
  b[0x26] = 0;   // shift
  for (size_t i = 0; i < parts.size(); ++i) {
    const size_t r = toc + rec * i, off = b.size();
    b[r + 1] = uint8_t(i + 1);
    b.insert(b.end(), parts[i].begin(), parts[i].end());
    put32(b, r + 2, uint32_t(off));
    put32(b, r + 6, uint32_t(parts[i].size()));
  }
  return b;
}

bool has(const std::vector<std::string>& v, const char* needle) {
  for (const auto& s : v)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

// A guest heap whose readable window is [lo, hi): span() refuses any range that leaves it (as the exe's fault-free
// span refuses an uncommitted page) and counts the refusals - the receipt never reads past a refusal.
struct Heap {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x40000, 0);
  uint32_t lo = 0x10000, hi = 0x40000;
  int refused = 0;
  void put(uint32_t va, const std::vector<uint8_t>& b) { std::memcpy(&mem[va], b.data(), b.size()); }
  static const uint8_t* span(void* u, uint32_t va, uint32_t n) {
    Heap* h = static_cast<Heap*>(u);
    if (n == 0 || va < h->lo || uint64_t(va) + n > h->hi) {
      ++h->refused;
      return nullptr;
    }
    return h->mem.data() + va;
  }
};

int runAssetReceiptTest() {
  std::printf("[asset-receipt-test] kernel/asset_receipt.hpp\n");
  const std::vector<ar::Candidate> cs = {cand("retail", 0x10), cand("override", 0x40)};
  bool aptOk = true;
  const uint32_t cstVA = 0x10000, aptVA = 0x20000, otherVA = 0x30000;

  // the matcher, through the span only
  {
    const auto& o = cs[1];
    Heap h;
    h.put(cstVA, o.cst);
    h.put(aptVA, o.apt);
    int k = ar::match(cs, &Heap::span, &h, aptVA, cstVA, &aptOk);
    check(k == 1 && aptOk, "R1 the override's .const and .apt: source override, apt=match");
    std::vector<uint8_t> apt2 = o.apt;
    apt2[250] ^= 1;   // a relocated .apt (a re-registration): the .const still decides
    Heap h2 = h;
    h2.put(aptVA, apt2);
    k = ar::match(cs, &Heap::span, &h2, aptVA, cstVA, &aptOk);
    check(k == 1 && !aptOk, "R2 the .const decides; a changed .apt is reported (apt=differs)");
    Heap h3;
    h3.put(cstVA, cs[0].cst);
    h3.put(aptVA, cs[0].apt);
    k = ar::match(cs, &Heap::span, &h3, aptVA, cstVA, &aptOk);
    check(k == 0 && aptOk, "R3 the disc's MAIN: source retail");
    std::vector<uint8_t> late = o.cst;
    late[150] ^= 1;   // equal for the first 64 bytes, differs after: no match (the whole .const is compared)
    Heap h4 = h;
    h4.put(cstVA, late);
    k = ar::match(cs, &Heap::span, &h4, aptVA, cstVA, &aptOk);
    check(k == -1, "R4 a .const that differs after the first 64 bytes matches nothing");
    const std::vector<uint8_t> other = part("Apt constant file\x1a", 0x77, 200);
    Heap h5 = h;
    h5.put(otherVA, other);
    k = ar::match(cs, &Heap::span, &h5, aptVA, otherVA, &aptOk);
    check(k == -1, "R5 another movie's .const matches nothing");
    Heap h6 = h;
    h6.hi = cstVA + uint32_t(o.cst.size()) - 1;   // the readable memory ends one byte before the candidate's end
    k = ar::match(cs, &Heap::span, &h6, aptVA, cstVA, &aptOk);
    check(k == -1 && h6.refused >= 1, "R6 a .const whose full length is not readable: no match, the read refused");
    k = ar::match(cs, nullptr, nullptr, aptVA, cstVA, &aptOk);
    check(k == -1 && !aptOk, "R7 no span: no match, nothing read");
    k = ar::match(cs, &Heap::span, &h, aptVA, 0, &aptOk);
    check(k == -1 && !aptOk, "R7b no .const pointer: no match");
    Heap h8 = h;
    h8.hi = aptVA + uint32_t(o.apt.size()) - 1;   // the .apt runs past the readable memory
    k = ar::match(cs, &Heap::span, &h8, aptVA, cstVA, &aptOk);
    check(k == 1 && !aptOk && h8.refused >= 1, "R8 an .apt whose full length is not readable: apt=differs, the read refused");
    // the worst case of Astra r3: a SHORT .const of another movie whose first 64 bytes equal the candidate's, right at
    // the end of readable memory - the prefix compares equal, the whole-length read is refused, no match
    Heap h9;
    const uint32_t edgeVA = 0x30000;
    h9.put(edgeVA, std::vector<uint8_t>(o.cst.begin(), o.cst.begin() + 64));
    h9.hi = edgeVA + 64;
    k = ar::match(cs, &Heap::span, &h9, aptVA, edgeVA, &aptOk);
    check(k == -1 && h9.refused >= 1, "R9 a short .const equal for 64 bytes at the readable edge: no match, no read past it");
    Heap h10;
    h10.lo = 0x20000;   // the .const address itself is not readable
    k = ar::match(cs, &Heap::span, &h10, aptVA, cstVA, &aptOk);
    check(k == -1 && h10.refused >= 1, "R10 an unreadable .const address: no match");
  }

  // onParse on a synthetic guest heap: the first registration matches nothing -> source=unknown once, and it takes
  // #1 (E3-E6 r6: the first registration owns the ordinal, so a later match is never a second "#1"); matches are
  // numbered per boot; a later unmatched registration (any other movie) logs nothing
  {
    ar::resetForTest();
    ar::setCandidates(cs, "[ASSET] MAIN expect source=override (test)");
    Heap h;
    const std::vector<uint8_t> other = part("Apt constant file\x1a", 0x77, 200);
    h.put(otherVA, other);
    h.put(cstVA, cs[1].cst);
    h.put(aptVA, cs[1].apt);
    ar::onParse(&Heap::span, &h, aptVA, otherVA);   // the first movie of the boot: neither candidate
    ar::onParse(&Heap::span, &h, aptVA, cstVA);     // the override
    ar::onParse(&Heap::span, &h, aptVA, otherVA);   // another movie: silent
    ar::onParse(&Heap::span, &h, aptVA, cstVA);     // a reload of MAIN: #3
    ar::onParse(&Heap::span, &h, 0, 0);             // no pointers: silent
    const auto lines = ar::loggedForTest();
    check(lines.size() == 4, "O1 four lines: the expectation, #1 unknown, #2, #3");
    check(has(lines, "[ASSET] MAIN expect source=override (test)"), "O2 the expectation line is logged");
    check(has(lines, "[ASSET] MAIN #1 source=unknown"), "O3 the first registration matching nothing: source=unknown");
    check(has(lines, "[ASSET] MAIN #2 source=override file=\"file-override\" sha256=oooo") &&
              has(lines, "const=match apt=match") && !has(lines, "[ASSET] MAIN #1 source=override"),
          "O4 the override's registration after an unknown first one: #2 source=override (never a second #1), its "
          "file and entry sha256, const / apt match");
    check(has(lines, "[ASSET] MAIN #3 source=override"), "O5 a re-registration is numbered #3");
  }
  {
    // O7 (E3-E6 r6): a boot whose first registration IS the override: #1, then a reload #2
    ar::resetForTest();
    ar::setCandidates(cs, "");
    Heap h;
    h.put(cstVA, cs[1].cst);
    h.put(aptVA, cs[1].apt);
    ar::onParse(&Heap::span, &h, aptVA, cstVA);
    ar::onParse(&Heap::span, &h, aptVA, cstVA);
    const auto lines = ar::loggedForTest();
    check(lines.size() == 2 && has(lines, "[ASSET] MAIN #1 source=override") &&
              has(lines, "[ASSET] MAIN #2 source=override") && !has(lines, "source=unknown"),
          "O7 the override registered first: #1 source=override, the reload #2, no unknown line");
  }
  {
    ar::resetForTest();
    ar::setCandidates({}, "");
    Heap h;
    ar::onParse(&Heap::span, &h, aptVA, cstVA);
    check(ar::loggedForTest().empty() && h.refused == 0, "O6 no candidates (no disc): onParse logs and reads nothing");
  }
  {
    ar::Candidate c;
    std::string err;
    const std::vector<uint8_t> junk(100, 0xAB);
    check(!ar::candidateFromEntry("MAIN", "retail", "x", junk, &c, &err) && !err.empty(),
          "C1 an entry that is not a BGFA container is refused with a reason");
  }
  {
    // a movie entry as the disc stores it: a nested BGFA1.05 container, the .const first, then the .apt
    const auto& o = cs[1];
    const std::vector<uint8_t> both = bgfa({o.cst, o.apt}), aptOnly = bgfa({o.apt});
    ar::Candidate c;
    std::string err;
    const bool ok = ar::candidateFromEntry("MAIN", "override", "f", both, &c, &err);
    check(ok && c.cst == o.cst && c.apt == o.apt && c.source == "override" && c.file == "f" && c.sha256.size() == 64,
          "C2 a movie entry: its .const and .apt told apart by their magics, the entry's sha256 kept");
    ar::Candidate c2;
    const bool ok2 = ar::candidateFromEntry("MAIN", "override", "f", bgfa({o.apt, o.cst}), &c2, &err);
    check(ok2 && c2.cst == o.cst && c2.apt == o.apt && c2.sha256 != c.sha256,
          "C3 the part order does not matter; a different entry has a different sha256");
    err.clear();
    check(!ar::candidateFromEntry("MAIN", "override", "f", aptOnly, &c, &err) && err.find("no .apt / .const") != std::string::npos,
          "C4 an entry without a .const is refused");
  }
  ar::resetForTest();
  std::printf("[asset-receipt-test] %s (%d passed, %d failed)\n", g_fail ? "FAIL" : "ALL PASS", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
